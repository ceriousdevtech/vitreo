// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus_graphics.cpp
/// @brief Contract tests for the GDI+ graphics area (gdiplus_graphics.cpp), run in
/// both bitnesses through gdiplus_fixture.hpp. Expected values come from
/// tools/oracle_probe/gdiplus_graphics_probe.expected (x64) / .x86.expected.

#include "gdiplus_fixture.hpp"

#include <winapi/graphics/gdiplus/gdiplus_geometry.hpp>
#include <winapi/graphics/gdiplus/gdiplus_graphics.hpp>
#include <winapi/graphics/gdiplus/gdiplus_image.hpp>
#include <winapi/graphics/gdiplus/gdiplus_render.hpp>
#include <winapi/core/thunk_registry.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <cmath>
#include <limits>

namespace vitreo::winapi::gdip_test {
namespace {

using gdip::GpGraphics;
using gdip::GpImage;
using gdip::GpMatrix;

class Gdip_graphics_Test : public GdipTest {
protected:
    static constexpr GuestAddr INPUT = DATA + 0x200;   ///< GdiplusStartupInput
    static constexpr GuestAddr OUTPUT = DATA + 0x240;  ///< GdiplusStartupOutput
    static constexpr GuestAddr BMI = DATA + 0x800;
    static constexpr GuestAddr PPV = DATA + 0x900;
    static constexpr GuestAddr THUNKS = 0x400000;

    void write_input(u32 version, bool suppress) {
        poison(INPUT, 32, 0);
        write_int(INPUT, static_cast<i32>(version));
        write_int(INPUT + (x64() ? 16 : 8), suppress ? 1 : 0);
    }
    Status startup(u32 version = 1, bool suppress = false, GuestAddr out = 0) {
        write_input(version, suppress);
        return call("GdiplusStartup", {I(OUT3), I(INPUT), I(out)});
    }
    void shutdown(u64 token) { (void)call("GdiplusShutdown", {I(token)}); }

    /// A memory DC with a w x h 32bpp top-down DIB section selected.
    Handle make_dc(i32 w, i32 h) {
        const Handle dc = gdi.create_compatible_dc(0);
        poison(BMI, 40, 0);
        write_int(BMI, 40);
        write_int(BMI + 4, w);
        write_int(BMI + 8, -h);
        EXPECT_TRUE(memory.write_u16(BMI + 12, 1).is_ok());
        EXPECT_TRUE(memory.write_u16(BMI + 14, 32).is_ok());
        const Handle dib = gdi.create_dib_section(dc, BMI, 0, PPV, x64());
        EXPECT_NE(dib, 0u);
        (void)gdi.select_object(dc, dib);
        return dc;
    }
    /// The DC surface pixel at (x, y) as R,G,B,A.
    std::array<u8, 4> dc_pixel(Handle dc, i32 x, i32 y) {
        std::array<u8, 4> px{};
        gdi.with_device_surface(dc, [&](GdiManager::DeviceSurface& s) {
            const u8* p = s.rgba + (static_cast<usize>(y) * static_cast<usize>(s.width) + static_cast<usize>(x)) * 4;
            px = {p[0], p[1], p[2], p[3]};
        });
        return px;
    }
    void fill_dc(Handle dc, std::array<u8, 4> rgba) {
        gdi.with_device_surface(dc, [&](GdiManager::DeviceSurface& s) {
            for (i32 i = 0; i < s.width * s.height; ++i) std::memcpy(s.rgba + i * 4, rgba.data(), 4);
        });
    }
    u64 from_hdc(Handle dc) {
        EXPECT_EQ(call("GdipCreateFromHDC", {I(dc), I(OUT)}), Status::Ok);
        return read_handle(OUT);
    }
    u64 hdc_graphics() { return from_hdc(make_dc(16, 16)); }
    GpGraphics* G(u64 h) { return mgr.objects().get<GpGraphics>(h); }

    /// An image graphics over a w x h 32bpp ARGB bitmap filled with @p argb.
    u64 image_graphics(i32 w, i32 h, u32 argb, u64* image_out = nullptr, float dpi_x = 96,
                       float dpi_y = 96) {
        GpImage img;
        img.width = w;
        img.height = h;
        img.stride = w * 4;
        img.dpi_x = dpi_x;
        img.dpi_y = dpi_y;
        img.bits.resize(static_cast<usize>(w * h * 4));
        for (i32 i = 0; i < w * h; ++i) std::memcpy(&img.bits[static_cast<usize>(i) * 4], &argb, 4);
        const u64 image = mgr.objects().add(std::move(img));
        if (image_out) *image_out = image;
        GpGraphics g;
        g.kind = gdip::TargetKind::Image;
        g.image = image;
        g.dpi_x = dpi_x;
        g.dpi_y = dpi_y;
        return mgr.objects().add(std::move(g));
    }
    u32 image_pixel(u64 image, i32 x, i32 y) {
        GpImage* img = mgr.objects().get<GpImage>(image);
        u32 v = 0;
        std::memcpy(&v, &img->bits[static_cast<usize>(y * img->stride + x * 4)], 4);
        return v;
    }
    u64 matrix(gdip::Affine a = {}) { return mgr.objects().add(GpMatrix{a}); }
    gdip::Affine world(u64 g) {
        const u64 mx = matrix();
        EXPECT_EQ(call("GdipGetWorldTransform", {I(g), I(mx)}), Status::Ok);
        return mgr.objects().get<GpMatrix>(mx)->m;
    }
    i32 get_int(const char* api, u64 g) {
        poison(OUT2, 4);
        EXPECT_EQ(call(api, {I(g), I(OUT2)}), Status::Ok) << api;
        return read_int(OUT2);
    }
    /// TransformPoints of one point; returns the status.
    Status tp(u64 g, i32 dst, i32 src, float x, float y, gdip::PointF& out) {
        write_reals(BUF, {x, y});
        const Status s = call("GdipTransformPoints", {I(g), I(dst), I(src), I(BUF), I(1)});
        out = {read_real(BUF), read_real(BUF + 4)};
        return s;
    }
    gdip::PointF to_device(u64 g, float x, float y) {
        gdip::PointF p;
        EXPECT_EQ(tp(g, 2, 0, x, y, p), Status::Ok);
        return p;
    }
};

#define EXPECT_PT(p, ex, ey)                       \
    do {                                           \
        EXPECT_NEAR((p).X, (ex), 1e-3f);           \
        EXPECT_NEAR((p).Y, (ey), 1e-3f);           \
    } while (0)

// ---- Startup ---------------------------------------------------------------

TEST_P(Gdip_graphics_Test, StartupValidatesArgumentsAndVersion) {
    write_input(1, false);
    EXPECT_EQ(call("GdiplusStartup", {I(0), I(INPUT), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdiplusStartup", {I(OUT3), I(0), I(0)}), Status::InvalidParameter);
    for (u32 v : {0u, 4u}) EXPECT_EQ(startup(v), Status::UnsupportedGdiplusVersion) << v;
    for (u32 v : {1u, 2u, 3u}) {
        poison(OUT3, 8, 0);
        ASSERT_EQ(startup(v), Status::Ok) << v;
        EXPECT_NE(read_handle(OUT3), 0u);
        shutdown(read_handle(OUT3));
    }
    // Without SuppressBackgroundThread the output is not written.
    poison(OUTPUT, 16);
    ASSERT_EQ(startup(1, false, OUTPUT), Status::Ok);
    EXPECT_EQ(memory.read_u32(OUTPUT).value(), 0xCCCCCCCCu);
    // Suppressing needs an output to receive the hooks.
    EXPECT_EQ(startup(1, true, 0), Status::InvalidParameter);
}

TEST_P(Gdip_graphics_Test, SuppressedStartupHandsOutCallableHooks) {
    ASSERT_TRUE(memory.map_region(THUNKS, 0x10000, MemoryProtection::ReadWrite).is_ok());
    ThunkRegistry thunks{THUNKS};
    mgr.set_thunk_registry(&thunks);
    ASSERT_TRUE(thunks.register_dll("gdiplus.dll", create_gdiplus_thunks(mgr, x64())).is_ok());
    handlers = gdiplus_handlers(mgr);

    // The export hook only works while a startup suppressed the thread.
    ASSERT_EQ(startup(), Status::Ok);
    const u64 plain = read_handle(OUT3);
    poison(OUT2, 8, 0);
    EXPECT_EQ(call("GdiplusNotificationHook", {I(OUT2)}), Status::GenericError);
    EXPECT_EQ(read_handle(OUT2), 0u);

    poison(OUTPUT, 16, 0);
    ASSERT_EQ(startup(1, true, OUTPUT), Status::Ok);
    const u64 hook = x64() ? memory.read_u64(OUTPUT).value() : memory.read_u32(OUTPUT).value();
    const u64 unhook = x64() ? memory.read_u64(OUTPUT + 8).value() : memory.read_u32(OUTPUT + 4).value();
    EXPECT_EQ(hook, thunks.get_thunk_address("gdiplus.dll", "Vitreo_GdiplusNotificationHook").value());
    EXPECT_EQ(unhook, thunks.get_thunk_address("gdiplus.dll", "Vitreo_GdiplusNotificationUnhook").value());
    // GDI+'s own entry points, not the same-named exports.
    EXPECT_NE(hook, thunks.get_thunk_address("gdiplus.dll", "GdiplusNotificationHook").value());

    for (const char* name : {"Vitreo_GdiplusNotificationHook", "GdiplusNotificationHook"}) {
        poison(OUT2, 8, 0);
        poison(BUF, 8, 0);
        ASSERT_EQ(call(name, {I(OUT2)}), Status::Ok) << name;
        ASSERT_EQ(call(name, {I(BUF)}), Status::Ok) << name;
        EXPECT_NE(read_handle(OUT2), 0u);
        EXPECT_NE(read_handle(OUT2), read_handle(BUF));
        (void)call("Vitreo_GdiplusNotificationUnhook", {I(read_handle(OUT2))});
        (void)call("GdiplusNotificationUnhook", {I(read_handle(BUF))});
    }
    shutdown(plain);
}

TEST_P(Gdip_graphics_Test, SuppressedStartupWithoutHookThunksFails) {
    // No thunk registry: reporting Ok would hand out NULL hooks.
    poison(OUTPUT, 16, 0);
    EXPECT_EQ(startup(1, true, OUTPUT), Status::GenericError);
    EXPECT_FALSE(gdip::gdiplus_started(mgr));
}

TEST_P(Gdip_graphics_Test, StartupIsReferenceCountedAndGatesGdipAlloc) {
    EXPECT_FALSE(gdip::gdiplus_started(mgr));
    (void)call("GdipAlloc", {I(8)});
    EXPECT_EQ(cpu.gpr.rax, 0u);  // NULL before startup
    ASSERT_EQ(startup(), Status::Ok);
    const u64 t1 = read_handle(OUT3);
    ASSERT_EQ(startup(), Status::Ok);
    const u64 t2 = read_handle(OUT3);
    EXPECT_NE(t1, t2);
    (void)call("GdipAlloc", {I(0)});
    const u64 p0 = cpu.gpr.rax;
    EXPECT_NE(p0, 0u);  // a zero size still allocates
    (void)call("GdipAlloc", {I(32)});
    const u64 p = cpu.gpr.rax;
    EXPECT_NE(p, 0u);
    EXPECT_EQ(p % 8, 0u);
    EXPECT_TRUE(memory.write_u32(p + 28, 0x12345678).is_ok());
    (void)call("GdipFree", {I(p)});
    (void)call("GdipFree", {I(p0)});
    (void)call("GdipFree", {I(0)});
    shutdown(t2);
    EXPECT_TRUE(gdip::gdiplus_started(mgr));
    shutdown(t1);
    EXPECT_FALSE(gdip::gdiplus_started(mgr));
    (void)call("GdipAlloc", {I(8)});
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_P(Gdip_graphics_Test, CreateStreamOnFileIsNotImplementedLikeWindows) {
    write_wide(BUF, u"C:\\file.bin");
    poison(OUT, 8, 0);
    EXPECT_EQ(call("GdipCreateStreamOnFile", {I(BUF), I(0x80000000u), I(OUT)}), Status::NotImplemented);
    EXPECT_EQ(read_handle(OUT), 0u);
    EXPECT_EQ(call("GdipCreateStreamOnFile", {I(0), I(0), I(0)}), Status::NotImplemented);
}

// ---- Creation and fresh state ---------------------------------------------

TEST_P(Gdip_graphics_Test, CreateFromHdcValidatesTheDc) {
    const Handle dc = make_dc(4, 4);
    EXPECT_EQ(call("GdipCreateFromHDC", {I(dc), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateFromHDC", {I(0), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreateFromHDC", {I(0x12345678), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreateFromHDC2", {I(dc), I(0), I(OUT)}), Status::Ok);
    EXPECT_NE(G(read_handle(OUT)), nullptr);
    EXPECT_EQ(call("GdipCreateFromHDC2", {I(0), I(0), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipDeleteGraphics", {I(0)}), Status::InvalidParameter);
    // A memory DC holding the default 1x1 monochrome bitmap is a valid target.
    EXPECT_EQ(call("GdipCreateFromHDC", {I(gdi.create_compatible_dc(0)), I(OUT)}), Status::Ok);
}

TEST_P(Gdip_graphics_Test, CreateFromHwndOwnsItsDc) {
    EXPECT_EQ(call("GdipCreateFromHWND", {I(0), I(0)}), Status::InvalidParameter);
    ASSERT_EQ(call("GdipCreateFromHWND", {I(0), I(OUT)}), Status::Ok);  // the screen
    const u64 g = read_handle(OUT);
    ASSERT_NE(G(g), nullptr);
    EXPECT_EQ(G(g)->kind, gdip::TargetKind::Hwnd);
    EXPECT_TRUE(G(g)->owns_hdc);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 3);
    poison(OUT2, 8, 0);
    ASSERT_EQ(call("GdipGetDC", {I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_handle(OUT2), G(g)->hdc);
    EXPECT_EQ(call("GdipReleaseDC", {I(g), I(read_handle(OUT2))}), Status::Ok);
    EXPECT_EQ(call("GdipDeleteGraphics", {I(g)}), Status::Ok);
    EXPECT_EQ(G(g), nullptr);
    ASSERT_EQ(call("GdipCreateFromHWNDICM", {I(0), I(OUT)}), Status::Ok);
    EXPECT_EQ(G(read_handle(OUT))->kind, gdip::TargetKind::Hwnd);
}

TEST_P(Gdip_graphics_Test, FreshGraphicsReportsWindowsDefaults) {
    const u64 image = image_graphics(8, 8, 0);
    for (const u64 g : {hdc_graphics(), image}) {
        EXPECT_EQ(get_int("GdipGetCompositingMode", g), 0);
        EXPECT_EQ(get_int("GdipGetCompositingQuality", g), 0);
        EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 3);       // None
        EXPECT_EQ(get_int("GdipGetInterpolationMode", g), 3);   // Bilinear
        EXPECT_EQ(get_int("GdipGetPixelOffsetMode", g), 0);
        EXPECT_EQ(get_int("GdipGetTextRenderingHint", g), 0);
        EXPECT_EQ(get_int("GdipGetTextContrast", g), 4);
        EXPECT_EQ(get_int("GdipGetPageUnit", g), 1);            // Display
        ASSERT_EQ(call("GdipGetPageScale", {I(g), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_real(OUT2), 1.0f);
        ASSERT_EQ(call("GdipGetDpiX", {I(g), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_real(OUT2), 96.0f);
        ASSERT_EQ(call("GdipGetDpiY", {I(g), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_real(OUT2), 96.0f);
        poison(OUT2, 8);
        ASSERT_EQ(call("GdipGetRenderingOrigin", {I(g), I(OUT2), I(OUT2 + 4)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), 0);
        EXPECT_EQ(read_int(OUT2 + 4), 0);
        EXPECT_TRUE(world(g).is_identity());
    }
}

TEST_P(Gdip_graphics_Test, GettersAndSettersRejectNullArguments) {
    const u64 g = hdc_graphics();
    for (const char* api : {"GdipGetCompositingMode", "GdipGetCompositingQuality", "GdipGetSmoothingMode",
                            "GdipGetInterpolationMode", "GdipGetPixelOffsetMode", "GdipGetTextRenderingHint",
                            "GdipGetTextContrast", "GdipGetPageUnit", "GdipGetPageScale", "GdipGetDpiX",
                            "GdipGetDpiY", "GdipGetNearestColor", "GdipSaveGraphics", "GdipBeginContainer2"}) {
        EXPECT_EQ(call(api, {I(0), I(OUT2)}), Status::InvalidParameter) << api;
        EXPECT_EQ(call(api, {I(g), I(0)}), Status::InvalidParameter) << api;
    }
    EXPECT_EQ(call("GdipGetRenderingOrigin", {I(g), I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetRenderingOrigin", {I(g), I(OUT2), I(0)}), Status::InvalidParameter);
    for (const char* api : {"GdipSetCompositingMode", "GdipSetCompositingQuality", "GdipSetSmoothingMode",
                            "GdipSetInterpolationMode", "GdipSetPixelOffsetMode", "GdipSetTextRenderingHint",
                            "GdipSetTextContrast", "GdipSetPageUnit"}) {
        EXPECT_EQ(call(api, {I(0), I(1)}), Status::InvalidParameter) << api;
    }
    EXPECT_EQ(call("GdipSetPageScale", {I(0), R(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipResetPageTransform", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipFlush", {I(0), I(0)}), Status::InvalidParameter);
    // A handle of another type is no graphics.
    EXPECT_EQ(call("GdipGetSmoothingMode", {I(matrix()), I(OUT2)}), Status::InvalidParameter);
}

// ---- Setters: range and read-back (probe "set <Mode>(v)") -------------------

TEST_P(Gdip_graphics_Test, ModeSettersValidateAndResolveAsWindows) {
    const u64 g = hdc_graphics();
    struct Case { const char* set; const char* get; i32 value; Status st; i32 back; };
    const Case cases[] = {
        {"GdipSetCompositingMode", "GdipGetCompositingMode", -1, Status::Ok, -1},
        {"GdipSetCompositingMode", "GdipGetCompositingMode", 100, Status::Ok, 100},
        {"GdipSetCompositingMode", "GdipGetCompositingMode", 1, Status::Ok, 1},
        {"GdipSetCompositingQuality", "GdipGetCompositingQuality", -1, Status::Ok, -1},
        {"GdipSetCompositingQuality", "GdipGetCompositingQuality", 100, Status::Ok, 100},
        {"GdipSetSmoothingMode", "GdipGetSmoothingMode", 0, Status::Ok, 3},
        {"GdipSetSmoothingMode", "GdipGetSmoothingMode", 1, Status::Ok, 3},
        {"GdipSetSmoothingMode", "GdipGetSmoothingMode", 2, Status::Ok, 4},
        {"GdipSetSmoothingMode", "GdipGetSmoothingMode", 5, Status::Ok, 5},
        {"GdipSetSmoothingMode", "GdipGetSmoothingMode", 6, Status::InvalidParameter, 5},
        {"GdipSetSmoothingMode", "GdipGetSmoothingMode", -1, Status::InvalidParameter, 5},
        {"GdipSetInterpolationMode", "GdipGetInterpolationMode", 0, Status::Ok, 3},
        {"GdipSetInterpolationMode", "GdipGetInterpolationMode", 1, Status::Ok, 3},
        {"GdipSetInterpolationMode", "GdipGetInterpolationMode", 2, Status::Ok, 7},
        {"GdipSetInterpolationMode", "GdipGetInterpolationMode", 5, Status::Ok, 5},
        {"GdipSetInterpolationMode", "GdipGetInterpolationMode", 8, Status::InvalidParameter, 5},
        {"GdipSetInterpolationMode", "GdipGetInterpolationMode", -1, Status::InvalidParameter, 5},
        {"GdipSetPixelOffsetMode", "GdipGetPixelOffsetMode", 4, Status::Ok, 4},
        {"GdipSetPixelOffsetMode", "GdipGetPixelOffsetMode", 0, Status::Ok, 0},
        {"GdipSetPixelOffsetMode", "GdipGetPixelOffsetMode", 5, Status::InvalidParameter, 0},
        {"GdipSetPixelOffsetMode", "GdipGetPixelOffsetMode", -1, Status::InvalidParameter, 0},
        {"GdipSetTextRenderingHint", "GdipGetTextRenderingHint", 5, Status::Ok, 5},
        {"GdipSetTextRenderingHint", "GdipGetTextRenderingHint", 6, Status::InvalidParameter, 5},
        {"GdipSetTextRenderingHint", "GdipGetTextRenderingHint", -1, Status::InvalidParameter, 5},
        {"GdipSetTextContrast", "GdipGetTextContrast", 12, Status::Ok, 12},
        {"GdipSetTextContrast", "GdipGetTextContrast", 0, Status::Ok, 0},
        {"GdipSetTextContrast", "GdipGetTextContrast", 13, Status::InvalidParameter, 0},
        {"GdipSetTextContrast", "GdipGetTextContrast", -1, Status::InvalidParameter, 0},
        {"GdipSetPageUnit", "GdipGetPageUnit", 6, Status::Ok, 6},
        {"GdipSetPageUnit", "GdipGetPageUnit", 0, Status::InvalidParameter, 6},
        {"GdipSetPageUnit", "GdipGetPageUnit", 7, Status::InvalidParameter, 6},
        {"GdipSetPageUnit", "GdipGetPageUnit", 2, Status::Ok, 2},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(call(c.set, {I(g), I(static_cast<u32>(c.value))}), c.st) << c.set << "(" << c.value << ")";
        EXPECT_EQ(get_int(c.get, g), c.back) << c.set << "(" << c.value << ")";
    }
}

TEST_P(Gdip_graphics_Test, PageScaleRangeRenderingOriginAndReset) {
    const u64 g = hdc_graphics();
    struct Case { float v; Status st; };
    const Case cases[] = {
        {0.0f, Status::InvalidParameter}, {-1.0f, Status::InvalidParameter},
        {1e-9f, Status::InvalidParameter}, {1.0000001e-9f, Status::Ok}, {0.5f, Status::Ok},
        {1e9f, Status::Ok}, {1.00000006e9f, Status::InvalidParameter}, {1e10f, Status::InvalidParameter},
        {std::numeric_limits<float>::quiet_NaN(), Status::Ok},
    };
    for (const Case& c : cases) EXPECT_EQ(call("GdipSetPageScale", {I(g), R(c.v)}), c.st) << c.v;
    ASSERT_EQ(call("GdipSetPageScale", {I(g), R(0.5f)}), Status::Ok);
    ASSERT_EQ(call("GdipGetPageScale", {I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 0.5f);

    ASSERT_EQ(call("GdipSetRenderingOrigin", {I(g), I(static_cast<u32>(-5)), I(7)}), Status::Ok);
    ASSERT_EQ(call("GdipGetRenderingOrigin", {I(g), I(OUT2), I(OUT2 + 4)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), -5);
    EXPECT_EQ(read_int(OUT2 + 4), 7);

    ASSERT_EQ(call("GdipSetPageUnit", {I(g), I(4)}), Status::Ok);
    ASSERT_EQ(call("GdipResetPageTransform", {I(g)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetPageUnit", g), 1);
    ASSERT_EQ(call("GdipGetPageScale", {I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 1.0f);
}

// ---- World transform ---------------------------------------------------------

TEST_P(Gdip_graphics_Test, WorldTransformOrderAndInvertibility) {
    const u64 g = hdc_graphics();
    EXPECT_EQ(call("GdipSetWorldTransform", {I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetWorldTransform", {I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetWorldTransform", {I(g), I(matrix({0, 0, 0, 0, 0, 0}))}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetWorldTransform", {I(g), I(matrix({1, 2, 2, 4, 0, 0}))}), Status::InvalidParameter);
    EXPECT_TRUE(world(g).is_identity());
    EXPECT_EQ(call("GdipSetWorldTransform", {I(g), I(matrix({1e-9f, 0, 0, 1e-9f, 0, 0}))}), Status::Ok);
    EXPECT_EQ(call("GdipSetWorldTransform", {I(g), I(matrix({1, 2, 3, 4, 5, 6}))}), Status::Ok);
    gdip::Affine w = world(g);
    EXPECT_EQ(w.m21, 3.0f);
    EXPECT_EQ(w.dy, 6.0f);

    ASSERT_EQ(call("GdipResetWorldTransform", {I(g)}), Status::Ok);
    ASSERT_EQ(call("GdipTranslateWorldTransform", {I(g), R(10), R(20), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipScaleWorldTransform", {I(g), R(2), R(3), I(1)}), Status::Ok);
    w = world(g);  // probe "T then S": [2 0 0 3 20 60]
    EXPECT_EQ(w.m11, 2.0f);
    EXPECT_EQ(w.m22, 3.0f);
    EXPECT_EQ(w.dx, 20.0f);
    EXPECT_EQ(w.dy, 60.0f);
    ASSERT_EQ(call("GdipRotateWorldTransform", {I(g), R(90), I(0)}), Status::Ok);
    w = world(g);  // probe "rot90 prepend": [~0 3 -2 ~0 20 60]
    EXPECT_NEAR(w.m11, 0, 1e-6);
    EXPECT_NEAR(w.m12, 3, 1e-5);
    EXPECT_NEAR(w.m21, -2, 1e-5);
    EXPECT_NEAR(w.m22, 0, 1e-6);
    ASSERT_EQ(call("GdipRotateWorldTransform", {I(g), R(30), I(1)}), Status::Ok);
    w = world(g);  // probe "rot30 append": [-1.5 2.59807 -1.73205 -1 -12.6795 61.9615]
    EXPECT_NEAR(w.m11, -1.5, 1e-4);
    EXPECT_NEAR(w.m12, 2.59807, 1e-4);
    EXPECT_NEAR(w.m21, -1.73205, 1e-4);
    EXPECT_NEAR(w.m22, -1, 1e-4);
    EXPECT_NEAR(w.dx, -12.6795, 1e-3);
    EXPECT_NEAR(w.dy, 61.9615, 1e-3);

    for (const char* api : {"GdipTranslateWorldTransform", "GdipScaleWorldTransform"}) {
        EXPECT_EQ(call(api, {I(g), R(1), R(1), I(2)}), Status::InvalidParameter) << api;
        EXPECT_EQ(call(api, {I(g), R(1), R(1), I(static_cast<u32>(-1))}), Status::InvalidParameter) << api;
    }
    EXPECT_EQ(call("GdipRotateWorldTransform", {I(g), R(1), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipMultiplyWorldTransform", {I(g), I(matrix()), I(2)}), Status::InvalidParameter);

    ASSERT_EQ(call("GdipResetWorldTransform", {I(g)}), Status::Ok);
    EXPECT_EQ(call("GdipScaleWorldTransform", {I(g), R(0), R(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipScaleWorldTransform", {I(g), R(0), R(1), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipRotateWorldTransform", {I(g), R(std::numeric_limits<float>::quiet_NaN()), I(0)}),
              Status::InvalidParameter);
    EXPECT_TRUE(world(g).is_identity());
    EXPECT_EQ(call("GdipMultiplyWorldTransform", {I(g), I(matrix({0, 0, 0, 0, 0, 0})), I(0)}),
              Status::InvalidParameter);
    // A NULL matrix multiplies as a successful no-op (probe "Mul(NULL m)=0").
    EXPECT_EQ(call("GdipMultiplyWorldTransform", {I(g), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipMultiplyWorldTransform", {I(g), I(g), I(0)}), Status::InvalidParameter);

    // probe "M then T prepend" / "T then M append": both [1 2 3 4 9 12].
    const u64 m6 = matrix({1, 2, 3, 4, 5, 6});
    ASSERT_EQ(call("GdipMultiplyWorldTransform", {I(g), I(m6), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipTranslateWorldTransform", {I(g), R(1), R(1), I(0)}), Status::Ok);
    w = world(g);
    EXPECT_EQ(w.dx, 9.0f);
    EXPECT_EQ(w.dy, 12.0f);
    ASSERT_EQ(call("GdipResetWorldTransform", {I(g)}), Status::Ok);
    ASSERT_EQ(call("GdipTranslateWorldTransform", {I(g), R(1), R(1), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipMultiplyWorldTransform", {I(g), I(m6), I(1)}), Status::Ok);
    w = world(g);
    EXPECT_EQ(w.dx, 9.0f);
    EXPECT_EQ(w.dy, 12.0f);
}

// ---- TransformPoints -------------------------------------------------------

TEST_P(Gdip_graphics_Test, TransformPointsBetweenSpaces) {
    const u64 g = hdc_graphics();
    ASSERT_EQ(call("GdipSetPageUnit", {I(g), I(4)}), Status::Ok);  // inch
    ASSERT_EQ(call("GdipSetPageScale", {I(g), R(2)}), Status::Ok);
    ASSERT_EQ(call("GdipTranslateWorldTransform", {I(g), R(1), R(2), I(0)}), Status::Ok);
    gdip::PointF p;
    // probe "inch2 TP(...)".
    ASSERT_EQ(tp(g, 2, 0, 1, 1, p), Status::Ok);
    EXPECT_PT(p, 384, 576);
    ASSERT_EQ(tp(g, 0, 2, 192, 96, p), Status::Ok);
    EXPECT_PT(p, 0, -1.5f);
    ASSERT_EQ(tp(g, 1, 0, 1, 1, p), Status::Ok);
    EXPECT_PT(p, 2, 3);
    ASSERT_EQ(tp(g, 0, 1, 1, 1, p), Status::Ok);
    EXPECT_PT(p, 0, -1);
    ASSERT_EQ(tp(g, 2, 1, 1, 1, p), Status::Ok);
    EXPECT_PT(p, 192, 192);
    ASSERT_EQ(tp(g, 1, 2, 192, 192, p), Status::Ok);
    EXPECT_PT(p, 1, 1);
    ASSERT_EQ(tp(g, 0, 0, 1, 1, p), Status::Ok);
    EXPECT_PT(p, 1, 1);
    EXPECT_EQ(tp(g, 3, 0, 1, 1, p), Status::InvalidParameter);
    EXPECT_EQ(tp(g, 0, 3, 1, 1, p), Status::InvalidParameter);
    EXPECT_EQ(tp(g, static_cast<i32>(-1), 0, 1, 1, p), Status::InvalidParameter);
    EXPECT_EQ(call("GdipTransformPoints", {I(g), I(2), I(0), I(BUF), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipTransformPoints", {I(g), I(2), I(0), I(BUF), I(static_cast<u32>(-1))}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipTransformPoints", {I(g), I(2), I(0), I(0), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipTransformPoints", {I(0), I(2), I(0), I(BUF), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipTransformPointsI", {I(g), I(2), I(0), I(BUF), I(0)}), Status::InvalidParameter);

    // Units at 96 dpi (probe "hdc unitN").
    ASSERT_EQ(call("GdipResetWorldTransform", {I(g)}), Status::Ok);
    ASSERT_EQ(call("GdipSetPageScale", {I(g), R(1)}), Status::Ok);
    const float per_unit[] = {1, 1, 96.0f / 72, 96, 96.0f / 300, 96.0f / 25.4f};
    for (i32 u = 1; u <= 6; ++u) {
        ASSERT_EQ(call("GdipSetPageUnit", {I(g), I(static_cast<u32>(u))}), Status::Ok);
        EXPECT_PT(to_device(g, 1, 1), per_unit[u - 1], per_unit[u - 1]);
    }
    // PixelOffsetModeHalf shifts device by -0.5 (probe "pom half").
    ASSERT_EQ(call("GdipSetPageUnit", {I(g), I(2)}), Status::Ok);
    ASSERT_EQ(call("GdipSetPixelOffsetMode", {I(g), I(4)}), Status::Ok);
    EXPECT_PT(to_device(g, 1, 1), 0.5f, 0.5f);
    ASSERT_EQ(call("GdipSetPixelOffsetMode", {I(g), I(2)}), Status::Ok);  // HighQuality = Half
    EXPECT_PT(to_device(g, 1, 1), 0.5f, 0.5f);
}

TEST_P(Gdip_graphics_Test, TransformPointsIRoundsHalfUp) {
    const u64 g = hdc_graphics();
    ASSERT_EQ(call("GdipScaleWorldTransform", {I(g), R(0.5f), R(0.5f), I(0)}), Status::Ok);
    write_ints(BUF, {1, 1, 3, 3, 5, 5, -1, -1, -3, -3, 7, -7});
    ASSERT_EQ(call("GdipTransformPointsI", {I(g), I(2), I(0), I(BUF), I(6)}), Status::Ok);
    // probe "TPI scale.5": 1,1 2,2 3,3 0,0 -1,-1 4,-3
    const i32 expect[] = {1, 1, 2, 2, 3, 3, 0, 0, -1, -1, 4, -3};
    for (i32 i = 0; i < 12; ++i) EXPECT_EQ(read_int(BUF + static_cast<u64>(i) * 4), expect[i]) << i;
}

TEST_P(Gdip_graphics_Test, ImageGraphicsUsesTheImageResolution) {
    const u64 g = image_graphics(8, 8, 0, nullptr, 72, 150);
    ASSERT_EQ(call("GdipGetDpiX", {I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 72.0f);
    ASSERT_EQ(call("GdipGetDpiY", {I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 150.0f);
    ASSERT_EQ(call("GdipSetPageUnit", {I(g), I(4)}), Status::Ok);
    EXPECT_PT(to_device(g, 1, 1), 72, 150);  // probe "img 72x150 inch"
    ASSERT_EQ(call("GdipSetPageUnit", {I(g), I(1)}), Status::Ok);
    EXPECT_PT(to_device(g, 1, 1), 1, 1);     // Display = pixel on an image
}

// ---- DC transform and alpha (RenderScope) -----------------------------------

TEST_P(Gdip_graphics_Test, HdcGraphicsCapturesTheDcTransformAtCreation) {
    const Handle dc = make_dc(32, 32);
    ASSERT_TRUE(gdi.set_viewport_org(dc, 10, 5));
    const u64 g = from_hdc(dc);
    EXPECT_PT(to_device(g, 0, 0), 10, 5);  // probe "vporg10,5 TP"
    // A later change to the DC does not move GDI+ drawing (probe "vporg
    // changed after create").
    ASSERT_TRUE(gdi.set_viewport_org(dc, 20, 20));
    EXPECT_PT(to_device(g, 0, 0), 10, 5);
    {
        gdip::RenderScope scope(mgr, marshaler, *G(g));
        ASSERT_EQ(scope.status(), Status::Ok);
        CGContextSetRGBFillColor(scope.cg(), 1, 0, 0, 1);
        CGContextFillRect(scope.cg(), CGRectMake(0, 0, 2, 2));
        EXPECT_EQ(scope.commit(), Status::Ok);
    }
    // probe "vporg10,5 fill first=10,5 last=11,6 count=4 px=ffff0000"
    EXPECT_EQ(dc_pixel(dc, 10, 5), (std::array<u8, 4>{0xFF, 0, 0, 0xFF}));
    EXPECT_EQ(dc_pixel(dc, 11, 6), (std::array<u8, 4>{0xFF, 0, 0, 0xFF}));
    // The drawing starts at the DC origin: nothing left of or above it. (The
    // far edge belongs to the rasterizer's aliased-fill rule, not this test.)
    EXPECT_EQ(dc_pixel(dc, 9, 5), (std::array<u8, 4>{0, 0, 0, 0}));
    EXPECT_EQ(dc_pixel(dc, 10, 4), (std::array<u8, 4>{0, 0, 0, 0}));
    EXPECT_EQ(dc_pixel(dc, 0, 0), (std::array<u8, 4>{0, 0, 0, 0}));
}

TEST_P(Gdip_graphics_Test, DrawingOnA32bppDibIsPremultipliedSourceOver) {
    // probe "alpha pre=XXXXXXXX fill=...": the DIB is taken as PARGB.
    struct Case { std::array<u8, 4> pre; float r, a; std::array<u8, 4> expect; };
    const Case cases[] = {
        {{0x33, 0x66, 0x99, 0x00}, 1, 1.0f, {0xFF, 0x00, 0x00, 0xFF}},
        {{0x33, 0x66, 0x99, 0x00}, 1, 128 / 255.0f, {0x99, 0x33, 0x4C, 0x80}},
        {{0x33, 0x66, 0x99, 0x12}, 1, 128 / 255.0f, {0x99, 0x33, 0x4C, 0x89}},
        {{0xFF, 0xFF, 0xFF, 0x10}, 0, 64 / 255.0f, {0xBF, 0xBF, 0xBF, 0x4C}},
    };
    for (const Case& c : cases) {
        const Handle dc = make_dc(8, 8);
        fill_dc(dc, c.pre);
        const u64 g = from_hdc(dc);
        {
            gdip::RenderScope scope(mgr, marshaler, *G(g));
            ASSERT_EQ(scope.status(), Status::Ok);
            CGContextSetRGBFillColor(scope.cg(), c.r, 0, 0, c.a);
            CGContextFillRect(scope.cg(), CGRectMake(0, 0, 4, 4));
        }
        const auto in = dc_pixel(dc, 1, 1);
        for (int k = 0; k < 4; ++k) EXPECT_NEAR(in[k], c.expect[k], 1) << k;
        EXPECT_EQ(dc_pixel(dc, 6, 6), c.pre);  // untouched outside the fill
    }
}

// ---- Save / Restore / containers ---------------------------------------------

TEST_P(Gdip_graphics_Test, SaveRestorePopsToTheMatchingToken) {
    const u64 g = hdc_graphics();
    ASSERT_EQ(call("GdipSaveGraphics", {I(g), I(OUT2)}), Status::Ok);
    const u32 s1 = static_cast<u32>(read_int(OUT2));
    ASSERT_EQ(call("GdipSetSmoothingMode", {I(g), I(4)}), Status::Ok);
    ASSERT_EQ(call("GdipSaveGraphics", {I(g), I(OUT2)}), Status::Ok);
    const u32 s2 = static_cast<u32>(read_int(OUT2));
    ASSERT_EQ(call("GdipSetSmoothingMode", {I(g), I(3)}), Status::Ok);
    ASSERT_EQ(call("GdipSaveGraphics", {I(g), I(OUT2)}), Status::Ok);
    const u32 s3 = static_cast<u32>(read_int(OUT2));
    EXPECT_NE(s1, s2);
    EXPECT_NE(s2, s3);
    ASSERT_EQ(call("GdipRestoreGraphics", {I(g), I(s2)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 4);
    // s3 was popped with s2; unknown tokens are successful no-ops.
    EXPECT_EQ(call("GdipRestoreGraphics", {I(g), I(s3)}), Status::Ok);
    EXPECT_EQ(call("GdipRestoreGraphics", {I(g), I(12345)}), Status::Ok);
    EXPECT_EQ(call("GdipRestoreGraphics", {I(g), I(0)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 4);
    ASSERT_EQ(call("GdipRestoreGraphics", {I(g), I(s1)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 3);
    EXPECT_TRUE(G(g)->saved.empty());
}

TEST_P(Gdip_graphics_Test, SaveAndContainerTokensDoNotCrossMatch) {
    const u64 g = hdc_graphics();
    // EndContainer ignores a save token; Restore ignores a container token.
    ASSERT_EQ(call("GdipSaveGraphics", {I(g), I(OUT2)}), Status::Ok);
    const u32 sv = static_cast<u32>(read_int(OUT2));
    ASSERT_EQ(call("GdipSetSmoothingMode", {I(g), I(4)}), Status::Ok);
    EXPECT_EQ(call("GdipEndContainer", {I(g), I(sv)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 4);
    EXPECT_EQ(call("GdipRestoreGraphics", {I(g), I(sv)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 3);

    ASSERT_EQ(call("GdipBeginContainer2", {I(g), I(OUT2)}), Status::Ok);
    const u32 c = static_cast<u32>(read_int(OUT2));
    ASSERT_EQ(call("GdipSetSmoothingMode", {I(g), I(4)}), Status::Ok);
    EXPECT_EQ(call("GdipRestoreGraphics", {I(g), I(c)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 4);
    EXPECT_EQ(call("GdipEndContainer", {I(g), I(c)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 3);

    // Ending a container pops a save made inside it.
    ASSERT_EQ(call("GdipBeginContainer2", {I(g), I(OUT2)}), Status::Ok);
    const u32 c2 = static_cast<u32>(read_int(OUT2));
    ASSERT_EQ(call("GdipSaveGraphics", {I(g), I(OUT2)}), Status::Ok);
    const u32 inner = static_cast<u32>(read_int(OUT2));
    ASSERT_EQ(call("GdipSetSmoothingMode", {I(g), I(4)}), Status::Ok);
    EXPECT_EQ(call("GdipEndContainer", {I(g), I(c2)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 3);
    EXPECT_EQ(call("GdipRestoreGraphics", {I(g), I(inner)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 3);
    EXPECT_TRUE(G(g)->saved.empty());
}

TEST_P(Gdip_graphics_Test, ContainerStartsFromDefaultsButKeepsTheRenderingOrigin) {
    const u64 g = hdc_graphics();
    ASSERT_EQ(call("GdipSetSmoothingMode", {I(g), I(4)}), Status::Ok);
    ASSERT_EQ(call("GdipSetCompositingMode", {I(g), I(1)}), Status::Ok);
    ASSERT_EQ(call("GdipSetCompositingQuality", {I(g), I(2)}), Status::Ok);
    ASSERT_EQ(call("GdipSetInterpolationMode", {I(g), I(5)}), Status::Ok);
    ASSERT_EQ(call("GdipSetPixelOffsetMode", {I(g), I(4)}), Status::Ok);
    ASSERT_EQ(call("GdipSetTextRenderingHint", {I(g), I(4)}), Status::Ok);
    ASSERT_EQ(call("GdipSetTextContrast", {I(g), I(7)}), Status::Ok);
    ASSERT_EQ(call("GdipSetRenderingOrigin", {I(g), I(3), I(4)}), Status::Ok);
    ASSERT_EQ(call("GdipSetPageUnit", {I(g), I(4)}), Status::Ok);
    ASSERT_EQ(call("GdipSetPageScale", {I(g), R(0.5f)}), Status::Ok);
    ASSERT_EQ(call("GdipTranslateWorldTransform", {I(g), R(5), R(6), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipBeginContainer2", {I(g), I(OUT3)}), Status::Ok);
    const u32 c = static_cast<u32>(read_int(OUT3));
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 3);
    EXPECT_EQ(get_int("GdipGetCompositingMode", g), 0);
    EXPECT_EQ(get_int("GdipGetCompositingQuality", g), 0);
    EXPECT_EQ(get_int("GdipGetInterpolationMode", g), 3);
    EXPECT_EQ(get_int("GdipGetPixelOffsetMode", g), 0);
    EXPECT_EQ(get_int("GdipGetTextRenderingHint", g), 0);
    EXPECT_EQ(get_int("GdipGetTextContrast", g), 4);
    EXPECT_EQ(get_int("GdipGetPageUnit", g), 1);
    EXPECT_TRUE(world(g).is_identity());
    ASSERT_EQ(call("GdipGetRenderingOrigin", {I(g), I(OUT2), I(OUT2 + 4)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(read_int(OUT2 + 4), 4);
    // probe "in container2 TP(2<-0)": (0,0) -> (239.5, 287.5), (1,2) -> +1,+2.
    EXPECT_PT(to_device(g, 0, 0), 239.5f, 287.5f);
    EXPECT_PT(to_device(g, 1, 2), 240.5f, 289.5f);
    gdip::PointF p;
    ASSERT_EQ(tp(g, 1, 0, 1, 2, p), Status::Ok);
    EXPECT_PT(p, 1, 2);
    ASSERT_EQ(call("GdipEndContainer", {I(g), I(c)}), Status::Ok);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 4);
    EXPECT_EQ(get_int("GdipGetCompositingMode", g), 1);
    EXPECT_EQ(get_int("GdipGetTextContrast", g), 7);
    EXPECT_EQ(get_int("GdipGetPageUnit", g), 4);
    EXPECT_EQ(world(g).dx, 5.0f);
}

TEST_P(Gdip_graphics_Test, Container2TakesTheWorldButNotThePageScale) {
    struct Case { i32 unit; float scale, sx, sy, tx, ty; i32 pom; float x0, y0, x1, y1; };
    // probe "c2 outer ...": device of container (0,0) and (1,0).
    const Case cases[] = {
        {2, 1, 2, 3, 0, 0, 0, 0, 0, 2, 0},
        {4, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0},
        {2, 2, 1, 1, 0, 0, 0, 0, 0, 1, 0},
        {4, 1, 1, 1, 5, 6, 0, 480, 576, 481, 576},
        {2, 1, 2, 2, 5, 6, 0, 5, 6, 7, 6},
        {2, 1, 1, 1, 0, 0, 4, -0.5f, -0.5f, 0.5f, -0.5f},
    };
    for (const Case& c : cases) {
        const u64 g = hdc_graphics();
        ASSERT_EQ(call("GdipSetPageUnit", {I(g), I(static_cast<u32>(c.unit))}), Status::Ok);
        ASSERT_EQ(call("GdipSetPageScale", {I(g), R(c.scale)}), Status::Ok);
        ASSERT_EQ(call("GdipTranslateWorldTransform", {I(g), R(c.tx), R(c.ty), I(0)}), Status::Ok);
        ASSERT_EQ(call("GdipScaleWorldTransform", {I(g), R(c.sx), R(c.sy), I(0)}), Status::Ok);
        ASSERT_EQ(call("GdipSetPixelOffsetMode", {I(g), I(static_cast<u32>(c.pom))}), Status::Ok);
        ASSERT_EQ(call("GdipBeginContainer2", {I(g), I(OUT3)}), Status::Ok);
        EXPECT_PT(to_device(g, 0, 0), c.x0, c.y0);
        EXPECT_PT(to_device(g, 1, 0), c.x1, c.y1);
        if (c.pom == 4) {  // probe "c2 outer+inner pom half": (-1,-1)
            ASSERT_EQ(call("GdipSetPixelOffsetMode", {I(g), I(4)}), Status::Ok);
            EXPECT_PT(to_device(g, 0, 0), -1, -1);
        }
    }
    // Rotation carries in; nesting composes (probe "c2 outer rot90", "nested c2").
    u64 g = hdc_graphics();
    ASSERT_EQ(call("GdipRotateWorldTransform", {I(g), R(90), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipBeginContainer2", {I(g), I(OUT3)}), Status::Ok);
    EXPECT_PT(to_device(g, 1, 0), 0, 1);
    g = hdc_graphics();
    ASSERT_EQ(call("GdipScaleWorldTransform", {I(g), R(2), R(2), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipBeginContainer2", {I(g), I(OUT3)}), Status::Ok);
    ASSERT_EQ(call("GdipScaleWorldTransform", {I(g), R(3), R(3), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipBeginContainer2", {I(g), I(OUT3)}), Status::Ok);
    EXPECT_PT(to_device(g, 1, 0), 6, 0);
}

TEST_P(Gdip_graphics_Test, RectContainerMapsSourceUnitsOntoDestination) {
    write_reals(BUF + 0x100, {10, 20, 100, 50});  // dst
    auto begin = [&](u64 g, std::initializer_list<float> src, i32 unit) {
        write_reals(BUF + 0x200, src);
        return call("GdipBeginContainer", {I(g), I(BUF + 0x100), I(BUF + 0x200),
                                           I(static_cast<u32>(unit)), I(OUT3)});
    };
    u64 g = hdc_graphics();
    ASSERT_EQ(begin(g, {0, 0, 50, 25}, 2), Status::Ok);
    EXPECT_PT(to_device(g, 0, 0), 10, 20);
    EXPECT_PT(to_device(g, 50, 25), 110, 70);
    gdip::PointF p;
    ASSERT_EQ(tp(g, 1, 0, 50, 25, p), Status::Ok);
    EXPECT_PT(p, 50, 25);
    ASSERT_EQ(call("GdipEndContainer", {I(g), I(read_int(OUT3))}), Status::Ok);
    // Units (probe "BeginContainer(unit N)": (50,25) -> ...).
    for (i32 u : {0, 1, 7}) EXPECT_EQ(begin(g, {0, 0, 50, 25}, u), Status::InvalidParameter) << u;
    const float expect[][2] = {{110, 70}, {85, 57.5f}, {11.0416f, 20.5208f}, {322.5f, 176.25f},
                               {36.4583f, 33.2291f}};
    for (i32 u = 2; u <= 6; ++u) {
        ASSERT_EQ(begin(g, {0, 0, 50, 25}, u), Status::Ok) << u;
        const gdip::PointF d = to_device(g, 50, 25);
        EXPECT_NEAR(d.X, expect[u - 2][0], 2e-3f) << u;
        EXPECT_NEAR(d.Y, expect[u - 2][1], 2e-3f) << u;
        ASSERT_EQ(call("GdipEndContainer", {I(g), I(read_int(OUT3))}), Status::Ok);
    }
    // Source offsets (probe "rc src offset point": (0,0)->(-10,10), (50,25)->(65,47.5)).
    ASSERT_EQ(begin(g, {10, 5, 50, 25}, 3), Status::Ok);
    EXPECT_PT(to_device(g, 0, 0), -10, 10);
    EXPECT_PT(to_device(g, 50, 25), 65, 47.5f);
    ASSERT_EQ(call("GdipEndContainer", {I(g), I(read_int(OUT3))}), Status::Ok);
    // Degenerate src maps identically (probe "src w0"); a zero-width dst
    // collapses x (probe "dst w0": (1,1) -> (10,22)).
    ASSERT_EQ(begin(g, {0, 0, 0, 25}, 2), Status::Ok);
    EXPECT_PT(to_device(g, 1, 1), 1, 1);
    ASSERT_EQ(call("GdipEndContainer", {I(g), I(read_int(OUT3))}), Status::Ok);
    write_reals(BUF + 0x100, {10, 20, 0, 50});
    ASSERT_EQ(begin(g, {0, 0, 50, 25}, 2), Status::Ok);
    EXPECT_PT(to_device(g, 1, 1), 10, 22);
    ASSERT_EQ(call("GdipEndContainer", {I(g), I(read_int(OUT3))}), Status::Ok);
    write_reals(BUF + 0x100, {10, 20, 100, 50});

    // The enclosing world AND page transform apply (probe "rc outer inch",
    // "rc outer pagescale2", "rc outer scale2,1 pixel").
    g = hdc_graphics();
    ASSERT_EQ(call("GdipSetPageUnit", {I(g), I(4)}), Status::Ok);
    ASSERT_EQ(begin(g, {0, 0, 50, 25}, 2), Status::Ok);
    EXPECT_PT(to_device(g, 0, 0), 960, 1920);
    EXPECT_PT(to_device(g, 50, 25), 10560, 6720);
    g = hdc_graphics();
    ASSERT_EQ(call("GdipScaleWorldTransform", {I(g), R(2), R(1), I(0)}), Status::Ok);
    ASSERT_EQ(begin(g, {0, 0, 50, 25}, 2), Status::Ok);
    EXPECT_PT(to_device(g, 50, 25), 220, 70);

    // Arguments.
    EXPECT_EQ(call("GdipBeginContainer", {I(g), I(0), I(BUF + 0x200), I(2), I(OUT3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBeginContainer", {I(g), I(BUF + 0x100), I(0), I(2), I(OUT3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBeginContainer", {I(g), I(BUF + 0x100), I(BUF + 0x200), I(2), I(0)}),
              Status::InvalidParameter);
    // The INT form (probe "in containerI": (50,25) -> (110,70)).
    g = hdc_graphics();
    write_ints(BUF + 0x300, {10, 20, 100, 50, 0, 0, 50, 25});
    ASSERT_EQ(call("GdipBeginContainerI", {I(g), I(BUF + 0x300), I(BUF + 0x310), I(2), I(OUT3)}), Status::Ok);
    EXPECT_PT(to_device(g, 50, 25), 110, 70);
}

// ---- GetDC / ReleaseDC -------------------------------------------------------

TEST_P(Gdip_graphics_Test, GetDcLocksTheGraphicsUntilReleased) {
    const Handle dc = make_dc(8, 8);
    const u64 g = from_hdc(dc);
    EXPECT_EQ(call("GdipGetDC", {I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetDC", {I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipReleaseDC", {I(g), I(dc)}), Status::InvalidParameter);  // none out
    poison(OUT2, 8, 0);
    ASSERT_EQ(call("GdipGetDC", {I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_handle(OUT2), dc);
    // probe "... while out": every graphics export answers ObjectBusy.
    EXPECT_EQ(call("GdipGetDC", {I(g), I(OUT2)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipGetSmoothingMode", {I(g), I(OUT3)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipSetSmoothingMode", {I(g), I(4)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipTranslateWorldTransform", {I(g), R(1), R(1), I(0)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipSaveGraphics", {I(g), I(OUT3)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipTransformPoints", {I(g), I(2), I(0), I(BUF), I(1)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipDeleteGraphics", {I(g)}), Status::ObjectBusy);
    {
        gdip::RenderScope scope(mgr, marshaler, *G(g));
        EXPECT_EQ(scope.status(), Status::ObjectBusy);
    }
    EXPECT_EQ(call("GdipReleaseDC", {I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipReleaseDC", {I(0), I(dc)}), Status::InvalidParameter);
    // Any non-NULL HDC ends the lock (probe "ReleaseDC(wrong)" 0).
    EXPECT_EQ(call("GdipReleaseDC", {I(g), I(0x1234)}), Status::Ok);
    EXPECT_EQ(call("GdipReleaseDC", {I(g), I(dc)}), Status::InvalidParameter);
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 3);
    EXPECT_EQ(call("GdipDeleteGraphics", {I(g)}), Status::Ok);
}

TEST_P(Gdip_graphics_Test, ImageGetDcWritesGdiDrawingBackOpaque) {
    u64 image = 0;
    const u64 g = image_graphics(8, 8, 0x80112233u, &image);
    ASSERT_EQ(call("GdipTranslateWorldTransform", {I(g), R(3), R(1), I(0)}), Status::Ok);
    poison(OUT2, 8, 0);
    ASSERT_EQ(call("GdipGetDC", {I(g), I(OUT2)}), Status::Ok);
    const Handle hdc = static_cast<Handle>(read_handle(OUT2));
    ASSERT_NE(hdc, 0u);
    // The DC holds the sentinel 0x000D0B0C everywhere, untransformed.
    EXPECT_EQ(dc_pixel(hdc, 0, 0), (std::array<u8, 4>{0x0D, 0x0B, 0x0C, 0x00}));
    EXPECT_EQ(dc_pixel(hdc, 7, 7), (std::array<u8, 4>{0x0D, 0x0B, 0x0C, 0x00}));
    EXPECT_EQ(call("GdipGetSmoothingMode", {I(g), I(OUT3)}), Status::ObjectBusy);
    gdi.with_device_surface(hdc, [](GdiManager::DeviceSurface& s) {
        auto put = [&](i32 x, i32 y, std::array<u8, 4> v) {
            std::memcpy(s.rgba + (y * s.width + x) * 4, v.data(), 4);
        };
        put(3, 3, {0xFF, 0x00, 0x00, 0x00});  // red, GDI's zero alpha
        put(7, 0, {0x00, 0x00, 0xFF, 0x7F});  // blue, any alpha
        put(1, 1, {0x0D, 0x0B, 0x0C, 0x01});  // the sentinel colour: alpha ignored
        put(2, 2, {0x0D, 0x0B, 0x0D, 0x00});  // near the sentinel: drawn
    });
    ASSERT_EQ(call("GdipReleaseDC", {I(g), I(hdc)}), Status::Ok);
    EXPECT_EQ(gdi.object_type(hdc), 0u);  // the DC is gone
    EXPECT_EQ(image_pixel(image, 3, 3), 0xFFFF0000u);
    EXPECT_EQ(image_pixel(image, 7, 0), 0xFF0000FFu);
    EXPECT_EQ(image_pixel(image, 2, 2), 0xFF0D0B0Du);
    // Untouched pixels keep the image value (through GDI+'s PARGB round trip).
    for (const auto& [x, y] : {std::pair{0, 0}, std::pair{1, 1}, std::pair{7, 7}}) {
        const u32 v = image_pixel(image, x, y);
        EXPECT_EQ(v >> 24, 0x80u);
        EXPECT_NEAR(static_cast<i32>((v >> 16) & 0xFF), 0x11, 1);
        EXPECT_NEAR(static_cast<i32>((v >> 8) & 0xFF), 0x22, 1);
        EXPECT_NEAR(static_cast<i32>(v & 0xFF), 0x33, 1);
    }
    EXPECT_EQ(get_int("GdipGetSmoothingMode", g), 3);
}

// ---- Miscellaneous -----------------------------------------------------------

TEST_P(Gdip_graphics_Test, NearestColorFlushCommentAbortAndTsRecord) {
    const u64 g = hdc_graphics();
    write_int(OUT2, static_cast<i32>(0x80123456u));
    ASSERT_EQ(call("GdipGetNearestColor", {I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0x80123456u);  // alpha kept
    for (u32 intention : {0u, 1u, 2u, 0xFFFFFFFFu}) {
        EXPECT_EQ(call("GdipFlush", {I(g), I(intention)}), Status::Ok) << intention;
    }
    write_ints(BUF, {0x04030201});
    EXPECT_EQ(call("GdipComment", {I(g), I(4), I(BUF)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipComment", {I(g), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipComment", {I(0), I(4), I(BUF)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipComment", {I(image_graphics(4, 4, 0)), I(4), I(BUF)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGraphicsSetAbort", {I(g), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipGraphicsSetAbort", {I(g), I(BUF)}), Status::Ok);
    EXPECT_EQ(G(g)->abort_object, BUF);
    EXPECT_EQ(call("GdipGraphicsSetAbort", {I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(static_cast<u32>(call("GdipPlayTSClientRecord", {I(g), I(0), I(0), I(0), I(0)})), 10011u);
    EXPECT_EQ(static_cast<u32>(call("GdipPlayTSClientRecord", {I(0), I(0), I(0), I(0), I(0)})), 10011u);
}

INSTANTIATE_GDIP_BITNESS(Gdip_graphics_Test);

}  // namespace
}  // namespace vitreo::winapi::gdip_test
