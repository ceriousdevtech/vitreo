// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus_image.cpp
/// @brief Contract tests for the GDI+ image area (gdiplus_image*.cpp), run
/// in both bitnesses through gdiplus_fixture.hpp. Expected values come from
/// tools/oracle_probe/gdiplus_image_probe.expected (x64) / .x86.expected;
/// each test names the probe line it checks.

#include "gdiplus_fixture.hpp"

#include <winapi/graphics/gdiplus/gdiplus_geometry.hpp>
#include <winapi/graphics/gdiplus/gdiplus_graphics.hpp>
#include <winapi/graphics/gdiplus/gdiplus_image.hpp>
#include <winapi/user32/resources/icon_planes.hpp>
#include <winapi/user32/resources/resources.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::winapi::gdip_test {
namespace {

using gdip::GpImage;

constexpr i32 F1 = gdip::PixelFormat1bppIndexed;
constexpr i32 F4 = gdip::PixelFormat4bppIndexed;
constexpr i32 F8 = gdip::PixelFormat8bppIndexed;
constexpr i32 FGray = gdip::PixelFormat16bppGrayScale;
constexpr i32 F555 = gdip::PixelFormat16bppRGB555;
constexpr i32 F565 = gdip::PixelFormat16bppRGB565;
constexpr i32 F1555 = gdip::PixelFormat16bppARGB1555;
constexpr i32 F24 = gdip::PixelFormat24bppRGB;
constexpr i32 F32 = gdip::PixelFormat32bppRGB;
constexpr i32 FArgb = gdip::PixelFormat32bppARGB;
constexpr i32 FPargb = gdip::PixelFormat32bppPARGB;
constexpr i32 F48 = gdip::PixelFormat48bppRGB;
constexpr i32 F64 = gdip::PixelFormat64bppARGB;
constexpr i32 F64p = gdip::PixelFormat64bppPARGB;
constexpr i32 FCmyk = gdip::PixelFormat32bppCMYK;

class Gdip_image_Test : public GdipTest {
protected:
    static constexpr GuestAddr BD = DATA + 0x400;    ///< BitmapData
    static constexpr GuestAddr RECT = DATA + 0x440;  ///< GpRect / GpRectF
    static constexpr GuestAddr PAL = DATA + 0x2000;  ///< ColorPalette (up to 256 entries)
    static constexpr GuestAddr CM = DATA + 0x3000;   ///< ColorMatrix / ColorMap
    static constexpr GuestAddr CM2 = DATA + 0x3100;
    static constexpr GuestAddr PIX = DATA + 0x4000;  ///< caller pixel buffers

    void SetUp() override {
        GdipTest::SetUp();
        mgr.set_resource_manager(&resources);
        // DIB section bits live in GDI's guest scratch arena.
        ASSERT_TRUE(memory.map_region(GdiManager::GDI_DIB_SCRATCH_BASE, GdiManager::GDI_DIB_SCRATCH_SIZE,
                                      MemoryProtection::ReadWrite).is_ok());
    }

    u64 bitmap(i32 w, i32 h, i32 fmt = FArgb) {
        EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(static_cast<u32>(w)), I(static_cast<u32>(h)), I(0),
                                                     I(static_cast<u32>(fmt)), I(0), I(OUT)}),
                  Status::Ok);
        return read_handle(OUT);
    }
    u64 argb(i32 w, i32 h, const std::vector<u32>& px) {
        const u64 b = bitmap(w, h);
        for (i32 y = 0; y < h; ++y)
            for (i32 x = 0; x < w; ++x)
                EXPECT_EQ(set_pixel(b, x, y, px[static_cast<usize>(y * w + x)]), Status::Ok);
        return b;
    }
    Status set_pixel(u64 b, i32 x, i32 y, u32 c) {
        return call("GdipBitmapSetPixel", {I(b), I(static_cast<u32>(x)), I(static_cast<u32>(y)), I(c)});
    }
    u32 get_pixel(u64 b, i32 x, i32 y, Status* st = nullptr) {
        poison(OUT2, 4);
        const Status s = call("GdipBitmapGetPixel", {I(b), I(static_cast<u32>(x)), I(static_cast<u32>(y)), I(OUT2)});
        if (st != nullptr) *st = s;
        return memory.read_u32(OUT2).value();
    }
    std::vector<u32> pixels(u64 b) {
        const u32 w = width(b), h = height(b);
        std::vector<u32> out;
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) out.push_back(get_pixel(b, static_cast<i32>(x), static_cast<i32>(y)));
        return out;
    }
    u32 width(u64 img) {
        EXPECT_EQ(call("GdipGetImageWidth", {I(img), I(OUT3)}), Status::Ok);
        return memory.read_u32(OUT3).value();
    }
    u32 height(u64 img) {
        EXPECT_EQ(call("GdipGetImageHeight", {I(img), I(OUT3)}), Status::Ok);
        return memory.read_u32(OUT3).value();
    }
    u32 format(u64 img) {
        EXPECT_EQ(call("GdipGetImagePixelFormat", {I(img), I(OUT3)}), Status::Ok);
        return memory.read_u32(OUT3).value();
    }
    void dispose(u64 img) { EXPECT_EQ(call("GdipDisposeImage", {I(img)}), Status::Ok); }

    // BitmapData accessors (Scan0 / Reserved are pointer-sized).
    u32 bd_width() { return memory.read_u32(BD).value(); }
    u32 bd_height() { return memory.read_u32(BD + 4).value(); }
    i32 bd_stride() { return read_int(BD + 8); }
    u32 bd_format() { return memory.read_u32(BD + 12).value(); }
    GuestAddr bd_scan0() { return read_handle(BD + 16); }
    void set_bd(u32 w, u32 h, i32 stride, u32 fmt, GuestAddr scan0) {
        poison(BD, 32, 0);
        write_int(BD, static_cast<i32>(w));
        write_int(BD + 4, static_cast<i32>(h));
        write_int(BD + 8, stride);
        write_int(BD + 12, static_cast<i32>(fmt));
        if (x64()) EXPECT_TRUE(memory.write_u64(BD + 16, scan0).is_ok());
        else EXPECT_TRUE(memory.write_u32(BD + 16, static_cast<u32>(scan0)).is_ok());
    }
    Status lock(u64 b, u32 mode, i32 fmt, GuestAddr rect = 0) {
        return call("GdipBitmapLockBits", {I(b), I(rect), I(mode), I(static_cast<u32>(fmt)), I(BD)});
    }
    Status unlock(u64 b) { return call("GdipBitmapUnlockBits", {I(b), I(BD)}); }
    std::string hex(GuestAddr at, usize n) {
        std::string s;
        char buf[4];
        for (usize i = 0; i < n; ++i) {
            std::snprintf(buf, sizeof(buf), "%02x", memory.read_u8(at + i).value());
            s += buf;
        }
        return s;
    }
    void write_bytes(GuestAddr at, const std::vector<u8>& v) {
        ASSERT_TRUE(memory.write_bytes(at, v.data(), v.size()).is_ok());
    }
    void write_matrix(GuestAddr at, const std::vector<float>& v) {
        for (usize i = 0; i < 25; ++i) write_real(at + 4 * i, i < v.size() ? v[i] : 0.0f);
    }
    std::vector<u32> palette_of(u64 img) {
        EXPECT_EQ(call("GdipGetImagePaletteSize", {I(img), I(OUT3)}), Status::Ok);
        const i32 size = read_int(OUT3);
        EXPECT_EQ(call("GdipGetImagePalette", {I(img), I(PAL), I(static_cast<u32>(size))}), Status::Ok);
        std::vector<u32> out;
        const u32 n = memory.read_u32(PAL + 4).value();
        for (u32 i = 0; i < n; ++i) out.push_back(memory.read_u32(PAL + 8 + 4 * i).value());
        return out;
    }
    /// Draw @p src (n x 1) into a fresh n x 1 ARGB bitmap: SourceCopy,
    /// NearestNeighbor, PixelOffsetModeHalf, through attributes @p ia (the
    /// probe's draw_attr).
    std::vector<u32> draw_attr(u64 ia, const std::vector<u32>& src) {
        const i32 n = static_cast<i32>(src.size());
        const u64 s = argb(n, 1, src);
        const u64 d = bitmap(n, 1);
        EXPECT_EQ(call("GdipGetImageGraphicsContext", {I(d), I(OUT)}), Status::Ok);
        const u64 g = read_handle(OUT);
        auto* gr = mgr.objects().get<gdip::GpGraphics>(g);
        gr->state.compositing_mode = 1;
        gr->state.interpolation_mode = 5;
        gr->state.pixel_offset_mode = 2;
        EXPECT_EQ(call("GdipDrawImageRectRectI", {I(g), I(s), I(0), I(0), I(static_cast<u32>(n)), I(1), I(0), I(0),
                                                  I(static_cast<u32>(n)), I(1), I(2), I(ia), I(0), I(0)}),
                  Status::Ok);
        (void)mgr.objects().remove<gdip::GpGraphics>(g);
        std::vector<u32> out = pixels(d);
        dispose(s);
        dispose(d);
        return out;
    }
    u64 attributes() {
        EXPECT_EQ(call("GdipCreateImageAttributes", {I(OUT)}), Status::Ok);
        return read_handle(OUT);
    }

    ResourceManager resources{memory};
};

using Px = std::vector<u32>;

// ---- Creation and getters ------------------------------------------------------

TEST_P(Gdip_image_Test, CreateValidatesArgumentsAsProbed) {
    // probe "== create"
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(0), I(2), I(0), I(FArgb), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(0), I(0), I(FArgb), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(0xFFFFFFFF), I(2), I(0), I(FArgb), I(0), I(OUT)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(0), I(FArgb), I(PIX), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(10), I(FArgb), I(PIX), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(4), I(FArgb), I(PIX), I(OUT)}), Status::Ok);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(static_cast<u32>(-8)), I(FArgb), I(PIX), I(OUT)}),
              Status::Ok);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(8), I(FArgb), I(0), I(OUT)}), Status::Ok);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(6), I(F24), I(PIX), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(3), I(2), I(12), I(F24), I(PIX), I(OUT)}), Status::Ok);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(0), I(0), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(0), I(0x12345678), I(0), I(OUT)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(0), I(0x0020000a), I(0), I(OUT)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(100000), I(100000), I(0), I(FArgb), I(0), I(OUT)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(0), I(FArgb), I(0), I(0)}), Status::InvalidParameter);
    // Bits 24-31 are ignored and reported back (probe "fmtcheck 8026200a").
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(1), I(0), I(0x8026200a), I(0), I(OUT)}), Status::Ok);
    EXPECT_EQ(format(read_handle()), 0x8026200au);
    EXPECT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(1), I(0), I(0x00a6200a), I(0), I(OUT)}),
              Status::InvalidParameter);
}

TEST_P(Gdip_image_Test, FreshBitmapGettersPerFormat) {
    // probe "== formats": flags, palette size, stride, initial pixel, graphics.
    struct Row { i32 fmt; u32 flags; i32 psize; i32 stride; Status graphics; };
    const Row rows[] = {
        {F1, 2, 16, 4, Status::Ok},       {F4, 2, 72, 4, Status::Ok},      {F8, 2, 1032, 4, Status::Ok},
        {FGray, 0, 12, 8, Status::OutOfMemory}, {F555, 0, 12, 8, Status::Ok}, {F565, 0, 12, 8, Status::Ok},
        {F1555, 2, 12, 8, Status::OutOfMemory}, {F24, 0, 12, 12, Status::Ok}, {F32, 0, 12, 12, Status::Ok},
        {FArgb, 2, 12, 12, Status::Ok},   {FPargb, 2, 12, 12, Status::Ok}, {F48, 0, 12, 20, Status::Ok},
        {F64, 2, 12, 24, Status::Ok},     {F64p, 2, 12, 24, Status::Ok},   {FCmyk, 0, 12, 12, Status::OutOfMemory},
    };
    for (const Row& r : rows) {
        SCOPED_TRACE(r.fmt);
        const u64 b = bitmap(3, 2, r.fmt);
        ASSERT_EQ(call("GdipGetImageFlags", {I(b), I(OUT3)}), Status::Ok);
        EXPECT_EQ(memory.read_u32(OUT3).value(), r.flags);
        ASSERT_EQ(call("GdipGetImagePaletteSize", {I(b), I(OUT3)}), Status::Ok);
        EXPECT_EQ(read_int(OUT3), r.psize);
        ASSERT_EQ(call("GdipGetImageHorizontalResolution", {I(b), I(OUT3)}), Status::Ok);
        EXPECT_EQ(read_real(OUT3), 96.0f);
        ASSERT_EQ(call("GdipGetImageType", {I(b), I(OUT3)}), Status::Ok);
        EXPECT_EQ(read_int(OUT3), 1);
        ASSERT_EQ(lock(b, 1, r.fmt), Status::Ok);
        EXPECT_EQ(bd_width(), 3u);
        EXPECT_EQ(bd_height(), 2u);
        EXPECT_EQ(bd_stride(), r.stride);
        EXPECT_EQ(bd_format(), static_cast<u32>(r.fmt));
        EXPECT_EQ(hex(bd_scan0(), 2), "0000");
        ASSERT_EQ(unlock(b), Status::Ok);
        EXPECT_EQ(call("GdipGetImageGraphicsContext", {I(b), I(OUT)}), r.graphics);
        dispose(b);
    }
}

TEST_P(Gdip_image_Test, DefaultPalettes) {
    // probe "fmt 00030101 ... pal=0 flags=2 count=2", "pal8_default".
    const u64 b1 = bitmap(2, 1, F1);
    EXPECT_EQ(palette_of(b1), (Px{0xff000000, 0xffffffff}));
    EXPECT_EQ(memory.read_u32(PAL).value(), 2u);
    const u64 b4 = bitmap(2, 1, F4);
    const Px p4 = palette_of(b4);
    ASSERT_EQ(p4.size(), 16u);
    EXPECT_EQ(p4[1], 0xff800000u);
    EXPECT_EQ(p4[8], 0xffc0c0c0u);
    EXPECT_EQ(p4[15], 0xffffffffu);
    EXPECT_EQ(memory.read_u32(PAL).value(), 0u);
    const u64 b8 = bitmap(2, 1, F8);
    const Px p8 = palette_of(b8);
    ASSERT_EQ(p8.size(), 256u);
    EXPECT_EQ(memory.read_u32(PAL).value(), 4u);
    EXPECT_EQ(p8[16], 0u);
    EXPECT_EQ(p8[39], 0u);
    EXPECT_EQ(p8[40], 0xff000000u);
    EXPECT_EQ(p8[41], 0xff000033u);
    EXPECT_EQ(p8[46], 0xff003300u);
    EXPECT_EQ(p8[255], 0xffffffffu);
    // A too-small buffer is InvalidParameter (probe "small=2"); a format
    // without a palette writes Count = 0 only (probe "pal24 flags=eeeeeeee").
    EXPECT_EQ(call("GdipGetImagePalette", {I(b1), I(PAL), I(15)}), Status::InvalidParameter);
    const u64 b24 = bitmap(2, 1, F24);
    poison(PAL, 12, 0xEE);
    EXPECT_EQ(call("GdipGetImagePalette", {I(b24), I(PAL), I(12)}), Status::Ok);
    EXPECT_EQ(memory.read_u32(PAL).value(), 0xEEEEEEEEu);
    EXPECT_EQ(memory.read_u32(PAL + 4).value(), 0u);
}

TEST_P(Gdip_image_Test, SetGetPixelQuantisesPerFormat) {
    // probe "setget" lines for each format.
    const Px in = {0xFF123456, 0x80FF4020, 0x00FFFFFF, 0x01020304, 0xFFFFFFFF, 0x7F808080, 0xFF000000, 0xC0C0C0C0};
    struct Row { i32 fmt; Px out; };
    const Row rows[] = {
        {F1, {0xff000000, 0xff000000, 0xffffffff, 0xff000000, 0xffffffff, 0xffffffff, 0xff000000, 0xffffffff}},
        {F4, {0xff000080, 0xffff0000, 0xffffffff, 0xff000000, 0xffffffff, 0xff808080, 0xff000000, 0xffc0c0c0}},
        {F8, {0xff003366, 0xffff3333, 0xffffffff, 0xff000000, 0xffffffff, 0xff808080, 0xff000000, 0xffcccccc}},
        {F555, {0xff103152, 0xffff4221, 0xffffffff, 0xff000000, 0xffffffff, 0xff848484, 0xff000000, 0xffc6c6c6}},
        {F565, {0xff103452, 0xffff4121, 0xffffffff, 0xff000000, 0xffffffff, 0xff848284, 0xff000000, 0xffc6c3c6}},
        {F1555, {0xff103152, 0xffff4221, 0x00ffffff, 0x00000000, 0xffffffff, 0x00848484, 0xff000000, 0xffc6c6c6}},
        {F24, {0xff123456, 0xffff4020, 0xffffffff, 0xff020304, 0xffffffff, 0xff808080, 0xff000000, 0xffc0c0c0}},
        {F32, {0xff123456, 0xffff4020, 0xffffffff, 0xff020304, 0xffffffff, 0xff808080, 0xff000000, 0xffc0c0c0}},
        {FArgb, in},
        {FPargb, {0xff123456, 0x80ff3f1f, 0x00000000, 0x01000000, 0xffffffff, 0x7f808080, 0xff000000, 0xc0c0c0c0}},
        {F48, {0xff123456, 0xffff4020, 0xffffffff, 0xff020304, 0xffffffff, 0xff808080, 0xff000000, 0xffc0c0c0}},
        {F64, {0xff123456, 0x7fff4020, 0x00ffffff, 0x00020304, 0xffffffff, 0x7e808080, 0xff000000, 0xbfc0c0c0}},
        {F64p, {0xff123456, 0x7fff4020, 0x00000000, 0x00000000, 0xffffffff, 0x7e808080, 0xff000000, 0xbfc0c0c0}},
    };
    for (const Row& r : rows) {
        SCOPED_TRACE(r.fmt);
        const u64 b = bitmap(3, 2, r.fmt);
        for (usize i = 0; i < in.size(); ++i) {
            ASSERT_EQ(set_pixel(b, 1, 1, in[i]), Status::Ok);
            Status st;
            EXPECT_EQ(get_pixel(b, 1, 1, &st), r.out[i]) << i;
            EXPECT_EQ(st, Status::Ok);
        }
        Status st;
        (void)get_pixel(b, 3, 0, &st);
        EXPECT_EQ(st, Status::InvalidParameter);
        EXPECT_EQ(set_pixel(b, 0, 2, 0), Status::InvalidParameter);
        (void)get_pixel(b, -1, 0, &st);
        EXPECT_EQ(st, Status::InvalidParameter);
        dispose(b);
    }
    for (i32 fmt : {FGray, FCmyk}) {
        const u64 b = bitmap(3, 2, fmt);
        Status st;
        EXPECT_EQ(get_pixel(b, 0, 0, &st), 0u);
        EXPECT_EQ(st, Status::InvalidParameter);
        EXPECT_EQ(set_pixel(b, 0, 0, 0xFF000000), Status::InvalidParameter);
        dispose(b);
    }
}

TEST_P(Gdip_image_Test, CallerBufferIsTheBitmap) {
    // probe "negstride_buf", "negstride_lock ... scan0_is_buf=1", "bufwrite_visible".
    poison(PIX, 128, 0);
    ASSERT_EQ(call("GdipCreateBitmapFromScan0", {I(2), I(2), I(static_cast<u32>(-8)), I(FArgb), I(PIX + 64), I(OUT)}),
              Status::Ok);
    const u64 b = read_handle();
    ASSERT_EQ(set_pixel(b, 0, 0, 0x11223344), Status::Ok);
    ASSERT_EQ(set_pixel(b, 1, 1, 0x55667788), Status::Ok);
    EXPECT_EQ(hex(PIX + 56, 16), "00000000887766554433221100000000");
    ASSERT_EQ(lock(b, 1, FArgb), Status::Ok);
    EXPECT_EQ(bd_stride(), -8);
    EXPECT_EQ(bd_scan0(), PIX + 64);
    ASSERT_EQ(unlock(b), Status::Ok);
    ASSERT_EQ(call("GdipCloneImage", {I(b), I(OUT)}), Status::Ok);
    const u64 clone = read_handle();
    ASSERT_TRUE(memory.write_u8(PIX + 64, 0x99).is_ok());
    EXPECT_EQ(get_pixel(b, 0, 0), 0x11223399u);
    EXPECT_EQ(get_pixel(clone, 0, 0), 0x11223399u);
}

// ---- LockBits ---------------------------------------------------------------------------

TEST_P(Gdip_image_Test, LockReadConvertsFromArgb) {
    // probe "read <fmt>=" lines (bytes past each row's pixels are not compared).
    const Px src = {0xFF123456, 0x80FF4020, 0x00FFFFFF, 0x40804020};
    struct Row { i32 fmt; i32 stride; std::string bytes; };
    const Row rows[] = {
        {F1, 4, "20"},
        {F4, 4, "49f3"},
        {F8, 4, "30e30f9b"},
        {F555, 8, "ca08047dff7f0441"},
        {F565, 8, "aa1104faffff0482"},
        {F1555, 8, "ca8804fdff7f0441"},
        {F24, 12, "5634122040ffffffff204080"},
        {F32, 16, "563412ff2040ff80ffffff0020408040"},
        {FArgb, 16, "563412ff2040ff80ffffff0020408040"},
        {FPargb, 16, "563412ff102080800000000008102040"},
        {F48, 24, "fa02190132007600a40100200020002000207600a401e806"},
        {F64, 32, "fa021901320000207600a4010020101000200020002000007600a401e8060808"},
        {F64p, 32, "fa021901320000203b00d2001010101000000000000000001d006900bb010808"},
    };
    for (const Row& r : rows) {
        SCOPED_TRACE(r.fmt);
        const u64 b = argb(4, 1, src);
        ASSERT_EQ(lock(b, 1, r.fmt), Status::Ok);
        EXPECT_EQ(bd_stride(), r.stride);
        EXPECT_EQ(hex(bd_scan0(), r.bytes.size() / 2), r.bytes);
        ASSERT_EQ(unlock(b), Status::Ok);
        dispose(b);
    }
    for (i32 fmt : {FGray, FCmyk}) {
        const u64 b = argb(4, 1, src);
        EXPECT_EQ(lock(b, 1, fmt), Status::InvalidParameter);
        dispose(b);
    }
}

TEST_P(Gdip_image_Test, LockWriteConvertsIntoArgb) {
    // probe "write <fmt>=" lines: the probe's byte pattern written into a
    // write-only lock of each format, read back with GetPixel.
    std::vector<u8> pat(64);
    for (usize k = 0; k < 64; ++k) pat[k] = static_cast<u8>(0x11 * (k % 15) + k);
    struct Row { i32 fmt; Px out; };
    const Row rows[] = {
        {F1, {0xff000000, 0xff000000, 0xff000000, 0xff000000}},
        {F4, {0xff000000, 0xff000000, 0xff800000, 0xff008000}},
        {F8, {0xff000000, 0x00000000, 0x00000000, 0xff006666}},
        {F555, {0xff218400, 0xff6b8c21, 0xffb59442, 0xffff9c63}},
        {F565, {0xff104100, 0xff31c721, 0xff5a4942, 0xff7bcf63}},
        {F1555, {0x00218400, 0x006b8c21, 0x00b59442, 0x00ff9c63}},
        {F24, {0xff241200, 0xff5a4836, 0xff907e6c, 0xffc6b4a2}},
        {F32, {0xff241200, 0xff6c5a48, 0xffb4a290, 0xfffcead8}},
        {FArgb, {0x36241200, 0x7e6c5a48, 0xc6b4a290, 0x0ffcead8}},
        {FPargb, {0x36a95400, 0x7edab691, 0xc6e7d0b9, 0x0fffffff}},
        {F48, {0xffffffc6, 0xff0000ff, 0xffffbb00, 0xff00ffff}},
        {F64, {0xffffffc6, 0x7f000000, 0x00ffffff, 0xffa50000}},
        {F64p, {0xffffffc6, 0x7f0000ff, 0x00ffffff, 0xffa50000}},
    };
    for (const Row& r : rows) {
        SCOPED_TRACE(r.fmt);
        const u64 b = bitmap(4, 1);
        ASSERT_EQ(lock(b, 2, r.fmt), Status::Ok);
        write_bytes(bd_scan0(), std::vector<u8>(pat.begin(), pat.begin() + bd_stride()));
        ASSERT_EQ(unlock(b), Status::Ok);
        EXPECT_EQ(pixels(b), r.out);
        dispose(b);
    }
    // A write-only 16bppGrayScale lock is granted; its unlock fails. CMYK
    // is refused at lock time (probe "write 00101004=0 unlock=2", "write 0000200f=2").
    const u64 b = bitmap(4, 1);
    ASSERT_EQ(lock(b, 2, FGray), Status::Ok);
    EXPECT_EQ(unlock(b), Status::InvalidParameter);
    EXPECT_EQ(lock(b, 2, FCmyk), Status::InvalidParameter);
}

TEST_P(Gdip_image_Test, LinearSixtyFourBitMapping) {
    // probe "to64" (sampled), "alpha64", "from64", "from64alpha".
    const std::pair<u32, u16> to64[] = {{0, 0}, {1, 2}, {2, 5}, {10, 25}, {11, 27}, {32, 118}, {64, 420},
                                       {128, 1768}, {200, 4732}, {254, 8119}, {255, 8192}};
    for (auto [v, want] : to64) {
        const u64 b = argb(1, 1, {0xFF000000u | (v << 16) | (v << 8) | v});
        ASSERT_EQ(lock(b, 1, F64), Status::Ok);
        EXPECT_EQ(memory.read_u16(bd_scan0()).value(), want) << v;
        ASSERT_EQ(unlock(b), Status::Ok);
        dispose(b);
    }
    {
        const u64 b = argb(1, 1, {0x0FFF8040});
        ASSERT_EQ(lock(b, 1, F64), Status::Ok);
        EXPECT_EQ(hex(bd_scan0(), 8), "a401e8060020e101");  // a15=420,1768,8192,481
        ASSERT_EQ(unlock(b), Status::Ok);
        ASSERT_EQ(lock(b, 1, F64p), Status::Ok);
        EXPECT_EQ(hex(bd_scan0(), 8), "18006700e101e101");  // p24,103,481,481
        ASSERT_EQ(unlock(b), Status::Ok);
        ASSERT_EQ(lock(b, 1, FPargb), Status::Ok);
        EXPECT_EQ(memory.read_u32(bd_scan0()).value(), 0x0f0f0804u);
        ASSERT_EQ(unlock(b), Status::Ok);
    }
    const std::pair<u16, u32> from64[] = {{0, 0x00}, {64, 0x16}, {128, 0x22}, {1024, 0x63}, {4096, 0xbc},
                                         {7104, 0xef}, {8192, 0xff}, {8448, 0xff}};
    for (auto [v, want] : from64) {
        const u64 b = bitmap(1, 1);
        ASSERT_EQ(lock(b, 2, F64), Status::Ok);
        const GuestAddr p = bd_scan0();
        for (int k = 0; k < 3; ++k) ASSERT_TRUE(memory.write_u16(p + 2 * k, v).is_ok());
        ASSERT_TRUE(memory.write_u16(p + 6, 8192).is_ok());
        ASSERT_EQ(unlock(b), Status::Ok);
        EXPECT_EQ(get_pixel(b, 0, 0) & 0xFF, want) << v;
        dispose(b);
    }
    const std::pair<u16, u32> from_alpha[] = {{0, 0x00bcbcbc}, {512, 0x0fbcbcbc}, {4096, 0x7fbcbcbc}, {8192, 0xffbcbcbc}};
    for (auto [a, want] : from_alpha) {
        const u64 b = bitmap(1, 1);
        ASSERT_EQ(lock(b, 2, F64), Status::Ok);
        const GuestAddr p = bd_scan0();
        for (int k = 0; k < 3; ++k) ASSERT_TRUE(memory.write_u16(p + 2 * k, 4096).is_ok());
        ASSERT_TRUE(memory.write_u16(p + 6, a).is_ok());
        ASSERT_EQ(unlock(b), Status::Ok);
        EXPECT_EQ(get_pixel(b, 0, 0), want) << a;
        dispose(b);
    }
    // 48bpp stores B, G, R linear WORDs (probe "48own=7600a401e806").
    const u64 b48 = bitmap(1, 1, F48);
    ASSERT_EQ(set_pixel(b48, 0, 0, 0xFF804020), Status::Ok);
    ASSERT_EQ(lock(b48, 1, F48), Status::Ok);
    EXPECT_EQ(hex(bd_scan0(), 6), "7600a401e806");
    ASSERT_EQ(unlock(b48), Status::Ok);
}

TEST_P(Gdip_image_Test, IndexedLocksAndPalettes) {
    // probe "8bpp_idx", "8bpp_as32", "8bpp_write32 ... idx=090a0c30", "1bpp", "4bpp".
    const u64 b8 = bitmap(4, 1, F8);
    ASSERT_EQ(lock(b8, 2, F8), Status::Ok);
    write_bytes(bd_scan0(), {0, 9, 40, 255});
    ASSERT_EQ(unlock(b8), Status::Ok);
    EXPECT_EQ(pixels(b8), (Px{0xff000000, 0xffff0000, 0xff000000, 0xffffffff}));
    ASSERT_EQ(lock(b8, 1, FArgb), Status::Ok);
    EXPECT_EQ(hex(bd_scan0(), 16), "000000ff0000ffff000000ffffffffff");
    ASSERT_EQ(unlock(b8), Status::Ok);
    EXPECT_EQ(lock(b8, 1, F4), Status::Ok);
    ASSERT_EQ(unlock(b8), Status::Ok);
    ASSERT_EQ(lock(b8, 2, FArgb), Status::Ok);
    for (u32 i = 0; i < 4; ++i) {
        const u32 v[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0x80123456};
        ASSERT_TRUE(memory.write_u32(bd_scan0() + 4 * i, v[i]).is_ok());
    }
    ASSERT_EQ(unlock(b8), Status::Ok);
    EXPECT_EQ(pixels(b8), (Px{0xffff0000, 0xff00ff00, 0xff0000ff, 0xff003366}));
    ASSERT_EQ(lock(b8, 1, F8), Status::Ok);
    EXPECT_EQ(hex(bd_scan0(), 4), "090a0c30");
    ASSERT_EQ(unlock(b8), Status::Ok);

    const u64 b1 = bitmap(9, 1, F1);
    ASSERT_EQ(lock(b1, 2, F1), Status::Ok);
    write_bytes(bd_scan0(), {0xA5, 0x80});
    ASSERT_EQ(unlock(b1), Status::Ok);
    EXPECT_EQ(pixels(b1), (Px{0xffffffff, 0xff000000, 0xffffffff, 0xff000000, 0xff000000, 0xffffffff, 0xff000000,
                              0xffffffff, 0xffffffff}));
    ASSERT_EQ(lock(b1, 2, FArgb), Status::Ok);
    for (u32 k = 0; k < 9; ++k)
        ASSERT_TRUE(memory.write_u32(bd_scan0() + 4 * k, (k & 1) ? 0xFF7F7F7F : 0xFF808080).is_ok());
    ASSERT_EQ(unlock(b1), Status::Ok);
    EXPECT_EQ(pixels(b1), (Px{0xffffffff, 0xff000000, 0xffffffff, 0xff000000, 0xffffffff, 0xff000000, 0xffffffff,
                              0xff000000, 0xffffffff}));

    const u64 b4 = bitmap(3, 1, F4);
    ASSERT_EQ(lock(b4, 2, F4), Status::Ok);
    write_bytes(bd_scan0(), {0x1F, 0x90});
    ASSERT_EQ(unlock(b4), Status::Ok);
    EXPECT_EQ(pixels(b4), (Px{0xff800000, 0xffffffff, 0xffff0000}));
}

TEST_P(Gdip_image_Test, LockRectRules) {
    // probe "rect(...)" lines and "mode" lines.
    Px px(16);
    for (u32 i = 0; i < 16; ++i) px[i] = 0xFF000000u | (i * 0x111111u);
    const u64 b = argb(4, 4, px);
    struct R { i32 x, y, w, h; Status s; u32 first; };
    const R rects[] = {{0, 0, 4, 4, Status::Ok, 0xff000000}, {1, 1, 2, 2, Status::Ok, 0xff555555},
                       {-1, 0, 2, 2, Status::InvalidParameter, 0}, {0, 0, 5, 4, Status::InvalidParameter, 0},
                       {0, 0, 0, 1, Status::InvalidParameter, 0}, {3, 3, 1, 1, Status::Ok, 0xffffffff},
                       {4, 0, 1, 1, Status::InvalidParameter, 0}, {0, 0, -1, 2, Status::InvalidParameter, 0}};
    for (const R& r : rects) {
        write_ints(RECT, {r.x, r.y, r.w, r.h});
        poison(BD, 32);
        ASSERT_EQ(lock(b, 1, FArgb, RECT), r.s);
        if (r.s != Status::Ok) {
            EXPECT_EQ(memory.read_u32(BD).value(), 0xCCCCCCCCu);
            continue;
        }
        EXPECT_EQ(bd_width(), static_cast<u32>(r.w));
        EXPECT_EQ(bd_stride(), 16);
        EXPECT_EQ(memory.read_u32(bd_scan0()).value(), r.first);
        EXPECT_NE(read_handle(BD + (x64() ? 24 : 20)), 0u);  // Reserved
        ASSERT_EQ(unlock(b), Status::Ok);
    }
    for (u32 mode : {0u, 1u, 2u, 3u, 5u, 7u, 8u, 0x10u}) {
        set_bd(4, 4, 16, FArgb, PIX);
        EXPECT_EQ(lock(b, mode, FArgb), Status::Ok) << mode;
        EXPECT_EQ(unlock(b), Status::Ok) << mode;
    }
}

TEST_P(Gdip_image_Test, LockUserInputBuffer) {
    // probe "userbuf_null", "userbuf_stride8", "userbuf_neg", "userbuf_write".
    Px px(16);
    for (u32 i = 0; i < 16; ++i) px[i] = 0xFF000000u | (i * 0x111111u);
    const u64 b = argb(4, 4, px);
    set_bd(0, 0, 0, 0, 0);
    EXPECT_EQ(lock(b, 1 | 4, FArgb), Status::InvalidParameter);
    poison(PIX, 256, 0);
    set_bd(0, 0, 8, 0, PIX);
    ASSERT_EQ(lock(b, 1 | 4, FArgb), Status::Ok);
    EXPECT_EQ(bd_width(), 4u);
    EXPECT_EQ(bd_stride(), 8);
    ASSERT_EQ(unlock(b), Status::Ok);
    EXPECT_EQ(memory.read_u32(PIX).value(), 0xff000000u);
    EXPECT_EQ(memory.read_u32(PIX + 4).value(), 0xff111111u);
    EXPECT_EQ(memory.read_u32(PIX + 8).value(), 0xff444444u);
    poison(PIX, 256, 0);
    set_bd(0, 0, -16, 0, PIX + 48 * 4);
    ASSERT_EQ(lock(b, 1 | 4, FArgb), Status::Ok);
    ASSERT_EQ(unlock(b), Status::Ok);
    EXPECT_EQ(memory.read_u32(PIX + 48 * 4).value(), 0xff000000u);
    EXPECT_EQ(memory.read_u32(PIX + 36 * 4).value(), 0xffccccccu);
    EXPECT_EQ(memory.read_u32(PIX).value(), 0u);
    for (u32 k = 0; k < 16; ++k) ASSERT_TRUE(memory.write_u32(PIX + 4 * k, 0x80000000u | k).is_ok());
    set_bd(0, 0, 16, 0, PIX);
    write_ints(RECT, {1, 1, 2, 2});
    ASSERT_EQ(lock(b, 2 | 4, FArgb, RECT), Status::Ok);
    ASSERT_EQ(unlock(b), Status::Ok);
    EXPECT_EQ(pixels(b), (Px{0xff000000, 0xff111111, 0xff222222, 0xff333333, 0xff444444, 0x80000000, 0x80000001,
                             0xff777777, 0xff888888, 0x80000004, 0x80000005, 0xffbbbbbb, 0xffcccccc, 0xffdddddd,
                             0xffeeeeee, 0xffffffff}));
}

TEST_P(Gdip_image_Test, LockStateRules) {
    // probe "locked: relock=8 getpixel=8 setpixel=8 width=0 ... graphics=0
    // clone=0 clonearea=0 hbitmap=0 setres=0", "unlock_again=7", "readlock_same_write".
    const u64 b = argb(2, 1, {0xFF102030, 0xFF405060});
    ASSERT_EQ(lock(b, 1, FArgb), Status::Ok);
    const GuestAddr scan0 = bd_scan0();
    EXPECT_EQ(lock(b, 1, FArgb), Status::WrongState);
    Status st;
    (void)get_pixel(b, 0, 0, &st);
    EXPECT_EQ(st, Status::WrongState);
    EXPECT_EQ(set_pixel(b, 0, 0, 0), Status::WrongState);
    EXPECT_EQ(width(b), 2u);
    EXPECT_EQ(call("GdipCloneImage", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(call("GdipBitmapSetResolution", {I(b), R(50), R(50)}), Status::Ok);
    ASSERT_TRUE(memory.write_u32(scan0, 0xFFABCDEF).is_ok());
    ASSERT_EQ(unlock(b), Status::Ok);
    EXPECT_EQ(unlock(b), Status::Win32Error);
    EXPECT_EQ(call("GdipBitmapUnlockBits", {I(b), I(0)}), Status::InvalidParameter);
    // Same-format read locks behave as GDI+'s direct pointer: writes stick.
    EXPECT_EQ(get_pixel(b, 0, 0), 0xffabcdefu);
    // Converted read locks are not written back; read+write ones are.
    ASSERT_EQ(lock(b, 1, F24), Status::Ok);
    ASSERT_TRUE(memory.write_u8(bd_scan0(), 0x01).is_ok());
    ASSERT_EQ(unlock(b), Status::Ok);
    EXPECT_EQ(get_pixel(b, 0, 0), 0xffabcdefu);
    ASSERT_EQ(lock(b, 3, F24), Status::Ok);
    EXPECT_EQ(hex(bd_scan0(), 3), "efcdab");
    ASSERT_TRUE(memory.write_u8(bd_scan0(), 0x01).is_ok());
    ASSERT_EQ(unlock(b), Status::Ok);
    EXPECT_EQ(get_pixel(b, 0, 0), 0xffabcd01u);
    // Rotation under a lock drops it (probe: every unlock after "rotate" is 7).
    ASSERT_EQ(lock(b, 1, FArgb), Status::Ok);
    EXPECT_EQ(call("GdipImageRotateFlip", {I(b), I(4)}), Status::Ok);
    EXPECT_EQ(unlock(b), Status::Win32Error);
    EXPECT_EQ(call("GdipBitmapLockBits", {I(b), I(0), I(1), I(FArgb), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapLockBits", {I(0), I(0), I(1), I(FArgb), I(BD)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapLockBits", {I(b), I(0), I(1), I(0), I(BD)}), Status::InvalidParameter);
}

// ---- Rotate / clone / resolution / palette ---------------------------------------------------

TEST_P(Gdip_image_Test, RotateFlipAllTypes) {
    // probe "rf0".."rf8".
    const Px src = {1, 2, 3, 4, 5, 6};
    const std::vector<std::pair<u32, Px>> want = {
        {3, {1, 2, 3, 4, 5, 6}}, {2, {4, 1, 5, 2, 6, 3}}, {3, {6, 5, 4, 3, 2, 1}}, {2, {3, 6, 2, 5, 1, 4}},
        {3, {3, 2, 1, 6, 5, 4}}, {2, {1, 4, 2, 5, 3, 6}}, {3, {4, 5, 6, 1, 2, 3}}, {2, {6, 3, 5, 2, 4, 1}}};
    for (u32 t = 0; t < 8; ++t) {
        Px in;
        for (u32 v : src) in.push_back(0xFF000000u | v);
        const u64 b = argb(3, 2, in);
        ASSERT_EQ(call("GdipImageRotateFlip", {I(b), I(t)}), Status::Ok);
        EXPECT_EQ(width(b), want[t].first) << t;
        Px out = pixels(b);
        for (u32& v : out) v &= 0xFF;
        EXPECT_EQ(out, want[t].second) << t;
        dispose(b);
    }
    const u64 b = bitmap(3, 2);
    EXPECT_EQ(call("GdipImageRotateFlip", {I(b), I(8)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipImageRotateFlip", {I(0), I(0)}), Status::InvalidParameter);
    // Indexed formats rotate too; the resolutions swap (probe "rf_4bpp ... dpi=100,50").
    const u64 b4 = bitmap(3, 2, F4);
    ASSERT_EQ(call("GdipBitmapSetResolution", {I(b4), R(50), R(100)}), Status::Ok);
    ASSERT_EQ(call("GdipImageRotateFlip", {I(b4), I(1)}), Status::Ok);
    EXPECT_EQ(width(b4), 2u);
    ASSERT_EQ(call("GdipGetImageHorizontalResolution", {I(b4), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_real(OUT3), 100.0f);
    EXPECT_EQ(format(b4), static_cast<u32>(F4));
}

TEST_P(Gdip_image_Test, CloneBitmapArea) {
    // probe "== clone".
    const u64 b = argb(4, 1, {0xFF123456, 0x80FF4020, 0x00FFFFFF, 0x40804020});
    ASSERT_EQ(call("GdipBitmapSetResolution", {I(b), R(120), R(60)}), Status::Ok);
    struct Row { i32 fmt; Status s; Px out; };
    const Row rows[] = {
        {FArgb, Status::Ok, {0x80ff4020, 0x00ffffff}}, {F24, Status::Ok, {0xffff4020, 0xffffffff}},
        {F8, Status::Ok, {0xffff3333, 0xffffffff}},    {F1, Status::Ok, {0xff000000, 0xffffffff}},
        {F555, Status::Ok, {0xffff4221, 0xffffffff}},  {F64p, Status::Ok, {0x7fff4020, 0x00000000}},
        {0, Status::Ok, {0x80ff4020, 0x00ffffff}},     {FGray, Status::OutOfMemory, {}},
        {FCmyk, Status::OutOfMemory, {}},
    };
    for (const Row& r : rows) {
        SCOPED_TRACE(r.fmt);
        ASSERT_EQ(call("GdipCloneBitmapAreaI", {I(1), I(0), I(2), I(1), I(static_cast<u32>(r.fmt)), I(b), I(OUT)}), r.s);
        if (r.s != Status::Ok) continue;
        const u64 c = read_handle();
        EXPECT_EQ(format(c), static_cast<u32>(r.fmt == 0 ? FArgb : r.fmt));
        ASSERT_EQ(call("GdipGetImageHorizontalResolution", {I(c), I(OUT3)}), Status::Ok);
        EXPECT_EQ(read_real(OUT3), 120.0f);
        EXPECT_EQ(pixels(c), r.out);
    }
    EXPECT_EQ(call("GdipCloneBitmapAreaI", {I(3), I(0), I(2), I(1), I(FArgb), I(b), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCloneBitmapAreaI", {I(static_cast<u32>(-1)), I(0), I(2), I(1), I(FArgb), I(b), I(OUT)}),
              Status::OutOfMemory);
    EXPECT_EQ(call("GdipCloneBitmapAreaI", {I(0), I(0), I(0), I(1), I(FArgb), I(b), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCloneBitmapAreaI", {I(0), I(0), I(1), I(1), I(0x12345678), I(b), I(OUT)}), Status::OutOfMemory);
    struct F { float x, w; Status s; Px out; };
    const F fs[] = {{0.5f, 2, Status::Ok, {0x80ff4020, 0x00ffffff}},
                    {0.4f, 2.6f, Status::Ok, {0xff123456, 0x80ff4020, 0x00ffffff}},
                    {1.5f, 2.5f, Status::OutOfMemory, {}},
                    {0.6f, 2.4f, Status::Ok, {0x80ff4020, 0x00ffffff}},
                    {3.4f, 1, Status::Ok, {0x40804020}}};
    for (const F& f : fs) {
        ASSERT_EQ(call("GdipCloneBitmapArea", {R(f.x), R(0), R(f.w), R(1), I(FArgb), I(b), I(OUT)}), f.s) << f.x;
        if (f.s == Status::Ok) EXPECT_EQ(pixels(read_handle()), f.out) << f.x;
    }
}

TEST_P(Gdip_image_Test, ResolutionBoundsDimensionAndMisc) {
    // probe "== misc".
    const u64 b = bitmap(3, 2);
    EXPECT_EQ(call("GdipBitmapSetResolution", {I(b), R(0), R(96)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapSetResolution", {I(b), R(-1), R(96)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapSetResolution", {I(b), R(72.5f), R(300)}), Status::Ok);
    ASSERT_EQ(call("GdipGetImageVerticalResolution", {I(b), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_real(OUT3), 300.0f);
    EXPECT_EQ(call("GdipBitmapSetResolution", {I(b), R(48), R(192)}), Status::Ok);
    ASSERT_EQ(call("GdipGetImageBounds", {I(b), I(RECT), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_real(RECT + 8), 3.0f);
    EXPECT_EQ(read_real(RECT + 12), 2.0f);
    EXPECT_EQ(read_int(OUT3), 2);
    ASSERT_EQ(call("GdipGetImageDimension", {I(b), I(OUT2), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 3.0f);
    EXPECT_EQ(read_real(OUT3), 2.0f);
    EXPECT_EQ(call("GdipImageForceValidation", {I(b)}), Status::Ok);
    EXPECT_EQ(call("GdipImageForceValidation", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipImageSetAbort", {I(b), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipImageSetAbort", {I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipFindFirstImageItem", {I(b), I(BD)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipFindNextImageItem", {I(b), I(BD)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetImageWidth", {I(0), I(OUT3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetImageHeight", {I(b), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetImageBounds", {I(b), I(RECT), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetImagePalette", {I(b), I(0), I(8)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCloneImage", {I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDisposeImage", {I(0)}), Status::InvalidParameter);
    // SetImagePalette stores any 1..256-entry palette (probe "setpal32",
    // "setpal8 ... 2x1 ff112233 ff112233", "setpal_count0=2", "setpal1_count4 psize=24").
    write_ints(PAL, {0, 2});
    write_ints(PAL + 8, {static_cast<i32>(0xFF112233), static_cast<i32>(0x80445566)});
    EXPECT_EQ(call("GdipSetImagePalette", {I(b), I(PAL)}), Status::Ok);
    ASSERT_EQ(call("GdipGetImagePaletteSize", {I(b), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 16);
    const u64 b8 = bitmap(2, 1, F8);
    EXPECT_EQ(call("GdipSetImagePalette", {I(b8), I(PAL)}), Status::Ok);
    EXPECT_EQ(pixels(b8), (Px{0xff112233, 0xff112233}));
    write_int(PAL + 4, 0);
    EXPECT_EQ(call("GdipSetImagePalette", {I(b8), I(PAL)}), Status::InvalidParameter);
    write_int(PAL + 4, 300);
    EXPECT_EQ(call("GdipSetImagePalette", {I(b8), I(PAL)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImagePalette", {I(b8), I(0)}), Status::InvalidParameter);
    const u64 b1 = bitmap(2, 1, F1);
    write_int(PAL + 4, 4);
    EXPECT_EQ(call("GdipSetImagePalette", {I(b1), I(PAL)}), Status::Ok);
    ASSERT_EQ(call("GdipGetImagePaletteSize", {I(b1), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 24);
}

TEST_P(Gdip_image_Test, BitmapFromGraphics) {
    // probe "fromgraphics=0 pf=000e200b dpi=120,144", "fromgraphics_zero=2".
    const u64 b = bitmap(3, 2);
    ASSERT_EQ(call("GdipBitmapSetResolution", {I(b), R(120), R(144)}), Status::Ok);
    ASSERT_EQ(call("GdipGetImageGraphicsContext", {I(b), I(OUT2)}), Status::Ok);
    const u64 g = read_handle(OUT2);
    ASSERT_EQ(call("GdipCreateBitmapFromGraphics", {I(5), I(4), I(g), I(OUT)}), Status::Ok);
    const u64 fg = read_handle();
    EXPECT_EQ(format(fg), static_cast<u32>(FPargb));
    ASSERT_EQ(call("GdipGetImageVerticalResolution", {I(fg), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_real(OUT3), 144.0f);
    EXPECT_EQ(get_pixel(fg, 4, 3), 0u);
    EXPECT_EQ(call("GdipCreateBitmapFromGraphics", {I(0), I(4), I(g), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromGraphics", {I(5), I(4), I(0), I(OUT)}), Status::InvalidParameter);
}

// ---- Image attributes ------------------------------------------------------------------------

const Px kAttrSrc = {0xFF808080, 0xFF123456, 0x80FF4020, 0xFF00FF00};

TEST_P(Gdip_image_Test, DrawWithoutAttributesRoundTripsThroughPargb) {
    EXPECT_EQ(draw_attr(0, kAttrSrc), (Px{0xff808080, 0xff123456, 0x80ff3f1f, 0xff00ff00}));  // probe "none"
    EXPECT_EQ(draw_attr(attributes(), kAttrSrc), (Px{0xff808080, 0xff123456, 0x80ff3f1f, 0xff00ff00}));
}

TEST_P(Gdip_image_Test, ColorMatrixResults) {
    // probe "scale", "translate", "swap", "invert", "m44_2", "col4", "skipgrays", "third", "alphahalf".
    const u64 ia = attributes();
    auto set = [&](const std::vector<float>& mat, u32 flags = 0, GuestAddr gray = 0) {
        write_matrix(CM, mat);
        return call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(1), I(CM), I(gray), I(flags)});
    };
    const std::vector<float> id = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    std::vector<float> m = id;
    m[0] = 0.5f; m[6] = 2; m[18] = 0.5f;
    ASSERT_EQ(set(m), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0x803fff7f, 0x80096755, 0x407f7f1f, 0x8000ff00}));
    m = id;
    m[20] = 0.2f; m[21] = -0.1f; m[22] = 0.05f;
    ASSERT_EQ(set(m), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xffb3668d, 0xff451a63, 0x80ff252d, 0xff33e50d}));
    const std::vector<float> inv = {-1, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    ASSERT_EQ(set(inv), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff7f7f7f, 0xffedcba9, 0x8000bfdf, 0xffff00ff}));
    m = id;
    m[24] = 2;
    m[4] = 0.5f;
    ASSERT_EQ(set(m), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff808080, 0xff123456, 0x80ff3f1f, 0xff00ff00}));
    ASSERT_EQ(set(inv, 1), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff808080, 0xffedcba9, 0x8000bfdf, 0xffff00ff}));
    m = id;
    m[0] = 1.0f / 3; m[6] = 2.0f / 3; m[12] = 0.1f;
    ASSERT_EQ(set(m), Status::Ok);
    EXPECT_EQ(draw_attr(ia, {0xFF010101, 0xFF020202, 0xFF050505, 0xFF7F7F7F, 0xFFFEFEFE, 0xFFFFFFFF}),
              (Px{0xff000100, 0xff010100, 0xff020301, 0xff2a550d, 0xff55a919, 0xff55aa1a}));
    m = id;
    m[18] = 0.5f;
    ASSERT_EQ(set(m), Status::Ok);
    EXPECT_EQ(draw_attr(ia, {0xFF010101, 0xFF020202, 0xFF050505, 0xFF7F7F7F, 0xFFFEFEFE, 0xFFFFFFFF}),
              (Px{0x80010101, 0x80010101, 0x80050505, 0x807f7f7f, 0x80fdfdfd, 0x80ffffff}));
    // Gray matrix (probe "gray_default", "gray_skip", "gray_alt", "gray_alt_red").
    const std::vector<float> half = {0.5f, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    write_matrix(CM2, half);
    const Px gsrc = {0xFF808080, 0xFF123456, 0x80404040, 0xFF818080, 0xFF000000};
    write_matrix(CM, inv);
    ASSERT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(1), I(CM), I(CM2), I(0)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, gsrc), (Px{0xff7f7f7f, 0xffedcba9, 0x80bfbfbf, 0xff7e7f7f, 0xffffffff}));
    ASSERT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(1), I(CM), I(CM2), I(1)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, gsrc), (Px{0xff808080, 0xffedcba9, 0x803f3f3f, 0xff7e7f7f, 0xff000000}));
    ASSERT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(1), I(CM), I(CM2), I(2)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, gsrc), (Px{0xff404040, 0xffedcba9, 0xff202020, 0xff7e7f7f, 0xff000000}));
    // Validation (probe "altgray_null=2 flags3=2 flags4=2 nullmatrix=2 disable_null=0 type5=2 type6=2 type_neg=2").
    EXPECT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(1), I(CM), I(0), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(1), I(CM), I(CM2), I(3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(1), I(0), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(0), I(0), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(5), I(1), I(CM), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(6), I(1), I(CM), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0xFFFFFFFF), I(1), I(CM), I(0), I(0)}),
              Status::InvalidParameter);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff808080, 0xff123456, 0x80ff3f1f, 0xff00ff00}));  // "after_disable"
}

TEST_P(Gdip_image_Test, AttributeCategoriesAndPipelineOrder) {
    const u64 ia = attributes();
    const std::vector<float> inv = {-1, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    const std::vector<float> swap = {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    write_matrix(CM, inv);
    write_matrix(CM2, swap);
    ASSERT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(1), I(CM), I(0), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(1), I(1), I(CM2), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff808080, 0xff563412, 0x801f3fff, 0xff00ff00}));
    ASSERT_EQ(call("GdipSetImageAttributesGamma", {I(ia), I(1), I(1), R(2.0f)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff404040, 0xff1d0b01, 0x80030fff, 0xff00ff00}));
    ASSERT_EQ(call("GdipResetImageAttributes", {I(ia), I(1)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff7f7f7f, 0xffedcba9, 0x8000bfdf, 0xffff00ff}));
    ASSERT_EQ(call("GdipSetImageAttributesNoOp", {I(ia), I(1), I(1)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff808080, 0xff123456, 0x80ff3f1f, 0xff00ff00}));
    ASSERT_EQ(call("GdipSetImageAttributesNoOp", {I(ia), I(1), I(0)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff808080, 0xff123456, 0x80ff3f1f, 0xff00ff00}));
    ASSERT_EQ(call("GdipSetImageAttributesToIdentity", {I(ia), I(1)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff808080, 0xff123456, 0x80ff3f1f, 0xff00ff00}));
    ASSERT_EQ(call("GdipResetImageAttributes", {I(ia), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipResetImageAttributes", {I(ia), I(1)}), Status::Ok);
    ASSERT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(2), I(1), I(CM), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff808080, 0xff123456, 0x80ff3f1f, 0xff00ff00}));  // "brush_only"
    ASSERT_EQ(call("GdipResetImageAttributes", {I(ia), I(2)}), Status::Ok);
    EXPECT_EQ(call("GdipResetImageAttributes", {I(ia), I(5)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesToIdentity", {I(ia), I(6)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesNoOp", {I(ia), I(7), I(1)}), Status::InvalidParameter);

    // Order: remap before key (probe "remap_key808080").
    const Px osrc = {0xFF808080, 0xFF123456, 0xFF404040, 0xFFC0C0C0};
    write_ints(CM, {static_cast<i32>(0xFF808080), static_cast<i32>(0xFF123456), static_cast<i32>(0xFF123456),
                    static_cast<i32>(0xFF808080)});
    ASSERT_EQ(call("GdipSetImageAttributesRemapTable", {I(ia), I(0), I(1), I(2), I(CM)}), Status::Ok);
    ASSERT_EQ(call("GdipSetImageAttributesColorKeys", {I(ia), I(0), I(1), I(0xFF808080), I(0xFF808080)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, osrc), (Px{0xff123456, 0x00000000, 0xff404040, 0xffc0c0c0}));
    ASSERT_EQ(call("GdipResetImageAttributes", {I(ia), I(0)}), Status::Ok);
    // Gamma before threshold (probe "thr0.4_gamma0.5").
    ASSERT_EQ(call("GdipSetImageAttributesThreshold", {I(ia), I(0), I(1), R(0.4f)}), Status::Ok);
    ASSERT_EQ(call("GdipSetImageAttributesGamma", {I(ia), I(0), I(1), R(0.5f)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, osrc), (Px{0xffffffff, 0xff00ffff, 0xffffffff, 0xffffffff}));
    ASSERT_EQ(call("GdipResetImageAttributes", {I(ia), I(0)}), Status::Ok);
    // Matrix before threshold (probe "thr0.6_inv"); key before threshold ("thr0.6_key000-404040").
    write_matrix(CM, inv);
    ASSERT_EQ(call("GdipSetImageAttributesThreshold", {I(ia), I(0), I(1), R(0.6f)}), Status::Ok);
    ASSERT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(1), I(CM), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, osrc), (Px{0xff000000, 0xffffffff, 0xffffffff, 0xff000000}));
    ASSERT_EQ(call("GdipResetImageAttributes", {I(ia), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipSetImageAttributesThreshold", {I(ia), I(0), I(1), R(0.6f)}), Status::Ok);
    ASSERT_EQ(call("GdipSetImageAttributesColorKeys", {I(ia), I(0), I(1), I(0xFF000000), I(0xFF404040)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, osrc), (Px{0xff000000, 0xff000000, 0x00000000, 0xffffffff}));
}

TEST_P(Gdip_image_Test, ThresholdGammaKeysRemap) {
    const u64 ia = attributes();
    // probe "threshold..." and the boundary lines "thr<t> base<b>".
    struct T { float t; Px out; };
    const T ts[] = {{0.0f, {0xffffffff, 0xffffffff, 0x80ffffff, 0xffffffff}},
                    {0.3f, {0xffffffff, 0xff0000ff, 0x80ff0000, 0xff00ff00}},
                    {1.0f, {0xff000000, 0xff000000, 0x80ff0000, 0xff00ff00}},
                    {-0.1f, {0xff000000, 0xff000000, 0x80ff0000, 0xff00ff00}},
                    {1.5f, {0xffffffff, 0xff000000, 0x80ff0000, 0xff00ff00}}};
    for (const T& t : ts) {
        ASSERT_EQ(call("GdipSetImageAttributesThreshold", {I(ia), I(0), I(1), R(t.t)}), Status::Ok);
        EXPECT_EQ(draw_attr(ia, kAttrSrc), t.out) << t.t;
    }
    struct B { float t; u32 base; Px out; };
    const B bs[] = {{0.3f, 75, {0xff000000, 0xff000000, 0xffffffff, 0xffffffff}},
                    {0.2f, 50, {0xff000000, 0xffffffff, 0xffffffff, 0xffffffff}},
                    {-0.1f, 229, {0xff000000, 0xff000000, 0xffffffff, 0xffffffff}},
                    {1.5f, 124, {0xff000000, 0xff000000, 0xff000000, 0xffffffff}},
                    {0.25f, 62, {0xff000000, 0xff000000, 0xffffffff, 0xffffffff}},
                    {2.0f, 253, {0xff000000, 0xffffffff, 0xffffffff, 0xffffffff}}};
    for (const B& b : bs) {
        Px src;
        for (u32 k = 0; k < 4; ++k) src.push_back(0xFF000000u | (std::min(b.base + k, 255u) * 0x010101u));
        ASSERT_EQ(call("GdipSetImageAttributesThreshold", {I(ia), I(0), I(1), R(b.t)}), Status::Ok);
        EXPECT_EQ(draw_attr(ia, src), b.out) << b.t;
    }
    ASSERT_EQ(call("GdipSetImageAttributesThreshold", {I(ia), I(0), I(0), R(0)}), Status::Ok);
    // probe "gamma..." lines.
    struct G { float g; Status s; Px out; };
    const G gs[] = {{0.5f, Status::Ok, {0xffb5b5b5, 0xff447394, 0x80ff7f59, 0xff00ff00}},
                    {2.2f, Status::Ok, {0xff383838, 0xff010817, 0x80ff0b03, 0xff00ff00}},
                    {0.0f, Status::InvalidParameter, {0xff383838, 0xff010817, 0x80ff0b03, 0xff00ff00}},
                    {-1.0f, Status::InvalidParameter, {0xff383838, 0xff010817, 0x80ff0b03, 0xff00ff00}},
                    {0.05f, Status::Ok, {0xfff6f6f6, 0xffdfecf2, 0x80ffede5, 0xff00ff00}},
                    {11.0f, Status::Ok, {0xff000000, 0xff000000, 0x80ff0000, 0xff00ff00}}};
    for (const G& g : gs) {
        EXPECT_EQ(call("GdipSetImageAttributesGamma", {I(ia), I(0), I(1), R(g.g)}), g.s) << g.g;
        EXPECT_EQ(draw_attr(ia, kAttrSrc), g.out) << g.g;
    }
    ASSERT_EQ(call("GdipResetImageAttributes", {I(ia), I(0)}), Status::Ok);
    // probe "colorkey", "colorkey_gray", "ck_lowgthigh=2", "ck_r=2 ck_g=2 ck_b=2 ck_a=0".
    ASSERT_EQ(call("GdipSetImageAttributesColorKeys", {I(ia), I(0), I(1), I(0x00120000), I(0x00FF5056)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff808080, 0x00000000, 0x00000000, 0xff00ff00}));
    ASSERT_EQ(call("GdipSetImageAttributesColorKeys", {I(ia), I(0), I(1), I(0xFF808080), I(0x00808080)}), Status::Ok);
    EXPECT_EQ(call("GdipSetImageAttributesColorKeys", {I(ia), I(0), I(1), I(0x00FFFFFF), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0x00000000, 0xff123456, 0x80ff3f1f, 0xff00ff00}));
    EXPECT_EQ(call("GdipSetImageAttributesColorKeys", {I(ia), I(0), I(1), I(0x00100000), I(0x000FFFFF)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesColorKeys", {I(ia), I(0), I(1), I(0x00000010), I(0x00FFFF0F)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesColorKeys", {I(ia), I(0), I(1), I(0xFF000000), I(0x00FFFFFF)}), Status::Ok);
    ASSERT_EQ(call("GdipResetImageAttributes", {I(ia), I(0)}), Status::Ok);
    // probe "remap", "remap_set=0 zero=2 null=2", "remap_alpha".
    write_ints(CM, {static_cast<i32>(0xFF123456), static_cast<i32>(0xFF00FF00), static_cast<i32>(0x80FF4020),
                    0x40000080, static_cast<i32>(0xFF00FF00), static_cast<i32>(0xFFFF0000)});
    ASSERT_EQ(call("GdipSetImageAttributesRemapTable", {I(ia), I(0), I(1), I(3), I(CM)}), Status::Ok);
    EXPECT_EQ(call("GdipSetImageAttributesRemapTable", {I(ia), I(2), I(1), I(0), I(CM)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesRemapTable", {I(ia), I(2), I(1), I(1), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(draw_attr(ia, kAttrSrc), (Px{0xff808080, 0xff00ff00, 0x4000007f, 0xffff0000}));
    write_ints(CM, {static_cast<i32>(0x80123456), static_cast<i32>(0xFF00FF00)});
    ASSERT_EQ(call("GdipSetImageAttributesRemapTable", {I(ia), I(0), I(1), I(1), I(CM)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia, {0xFF123456, 0x80123456}), (Px{0xff123456, 0xff00ff00}));
}

TEST_P(Gdip_image_Test, OutputChannelAndProfileValidation) {
    const u64 ia = attributes();
    // Separations are colour-managed (not byte-exact with Windows): check
    // the measured invariants: gray output, alpha kept, white has no ink,
    // full-ink primaries (probe "chan0".."chan3").
    const Px csrc = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFF000000, 0xFFFFFFFF, 0x80FFFF00};
    for (u32 ch = 0; ch < 4; ++ch) {
        ASSERT_EQ(call("GdipSetImageAttributesOutputChannel", {I(ia), I(0), I(1), I(ch)}), Status::Ok);
        const Px out = draw_attr(ia, csrc);
        for (u32 v : out) EXPECT_TRUE(((v >> 16) & 0xFF) == ((v >> 8) & 0xFF) && ((v >> 8) & 0xFF) == (v & 0xFF)) << std::hex << v;
        EXPECT_EQ(out[4], 0xffffffffu) << ch;
        EXPECT_EQ(out[5] >> 24, 0x80u) << ch;
    }
    EXPECT_LT(draw_attr(ia, {0xFF000000})[0] & 0xFF, 0x40u);  // K of black is near full ink
    EXPECT_EQ(call("GdipSetImageAttributesOutputChannel", {I(ia), I(0), I(1), I(4)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetImageAttributesOutputChannelColorProfile", {I(ia), I(0), I(1), I(0)}), Status::Win32Error);
    write_wide(BUF, u"nope.icc");
    EXPECT_EQ(call("GdipSetImageAttributesOutputChannelColorProfile", {I(ia), I(0), I(1), I(BUF)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipSetImageAttributesOutputChannelColorProfile", {I(ia), I(0), I(0), I(0)}), Status::Ok);
}

TEST_P(Gdip_image_Test, WrapModesAndCloneAndAdjustedPalette) {
    // probe "wrap0".."wrap4", "wrap_none".
    const u64 s = argb(2, 1, {0xFF0000FF, 0xFFFF0000});
    const Px want[5] = {{0xff0000ff, 0xffff0000, 0xff0000ff, 0xffff0000, 0xff0000ff, 0xffff0000},
                        {0xffff0000, 0xff0000ff, 0xff0000ff, 0xffff0000, 0xffff0000, 0xff0000ff},
                        {0xff0000ff, 0xffff0000, 0xff0000ff, 0xffff0000, 0xff0000ff, 0xffff0000},
                        {0xffff0000, 0xff0000ff, 0xff0000ff, 0xffff0000, 0xffff0000, 0xff0000ff},
                        {0x8000ff00, 0x8000ff00, 0xff0000ff, 0xffff0000, 0x8000ff00, 0x8000ff00}};
    for (u32 wm = 0; wm <= 5; ++wm) {
        const u64 d = bitmap(6, 1);
        ASSERT_EQ(call("GdipGetImageGraphicsContext", {I(d), I(OUT)}), Status::Ok);
        const u64 g = read_handle(OUT);
        auto* gr = mgr.objects().get<gdip::GpGraphics>(g);
        gr->state.compositing_mode = 1;
        gr->state.interpolation_mode = 5;
        gr->state.pixel_offset_mode = 2;
        const u64 ia = attributes();
        ASSERT_EQ(call("GdipSetImageAttributesWrapMode", {I(ia), I(wm), I(0x8000FF00), I(0)}), Status::Ok);
        const u64 use = wm == 5 ? 0 : ia;
        ASSERT_EQ(call("GdipDrawImageRectRectI", {I(g), I(s), I(0), I(0), I(6), I(1), I(static_cast<u32>(-2)), I(0),
                                                  I(6), I(1), I(2), I(use), I(0), I(0)}),
                  Status::Ok);
        if (wm < 5) EXPECT_EQ(pixels(d), want[wm]) << wm;
        else EXPECT_EQ(pixels(d), (Px{0, 0, 0xff0000ff, 0xffff0000, 0, 0}));
    }
    // Clone keeps state (probe "clone_inv"); the adjusted palette uses the
    // category's fallback to Default and refuses Default itself ("adjpal*").
    const u64 ia = attributes();
    const std::vector<float> inv = {-1, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    write_matrix(CM, inv);
    ASSERT_EQ(call("GdipSetImageAttributesColorMatrix", {I(ia), I(0), I(1), I(CM), I(0), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipCloneImageAttributes", {I(ia), I(OUT2)}), Status::Ok);
    const u64 ia2 = read_handle(OUT2);
    ASSERT_EQ(call("GdipResetImageAttributes", {I(ia), I(0)}), Status::Ok);
    EXPECT_EQ(draw_attr(ia2, kAttrSrc), (Px{0xff7f7f7f, 0xffedcba9, 0x8000bfdf, 0xffff00ff}));
    write_ints(PAL, {0, 4, static_cast<i32>(0xFF808080), static_cast<i32>(0xFF123456), static_cast<i32>(0x80FF4020),
                     static_cast<i32>(0xFF00FF00)});
    ASSERT_EQ(call("GdipGetImageAttributesAdjustedPalette", {I(ia2), I(PAL), I(1)}), Status::Ok);
    EXPECT_EQ(memory.read_u32(PAL + 8).value(), 0xff7f7f7fu);
    EXPECT_EQ(memory.read_u32(PAL + 12).value(), 0xffedcba9u);
    EXPECT_EQ(memory.read_u32(PAL + 16).value(), 0x8000bfdfu);
    EXPECT_EQ(memory.read_u32(PAL + 20).value(), 0xffff00ffu);
    EXPECT_EQ(call("GdipGetImageAttributesAdjustedPalette", {I(ia2), I(PAL), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetImageAttributesAdjustedPalette", {I(ia2), I(0), I(1)}), Status::InvalidParameter);
    write_int(PAL + 4, 0);
    EXPECT_EQ(call("GdipGetImageAttributesAdjustedPalette", {I(ia2), I(PAL), I(1)}), Status::InvalidParameter);
    write_int(PAL + 4, 1);
    EXPECT_EQ(call("GdipGetImageAttributesAdjustedPalette", {I(ia2), I(PAL), I(5)}), Status::InvalidParameter);
    ASSERT_EQ(call("GdipSetImageAttributesColorKeys", {I(ia2), I(1), I(1), I(0), I(0x00808080)}), Status::Ok);
    write_ints(PAL, {0, 2, static_cast<i32>(0xFF808080), static_cast<i32>(0xFFC0C0C0)});
    ASSERT_EQ(call("GdipGetImageAttributesAdjustedPalette", {I(ia2), I(PAL), I(1)}), Status::Ok);
    EXPECT_EQ(memory.read_u32(PAL + 8).value(), 0x00808080u);
    EXPECT_EQ(memory.read_u32(PAL + 12).value(), 0xffc0c0c0u);
    EXPECT_EQ(call("GdipSetImageAttributesCachedBackground", {I(ia2), I(1)}), Status::Ok);
    EXPECT_EQ(call("GdipDisposeImageAttributes", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateImageAttributes", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCloneImageAttributes", {I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDisposeImageAttributes", {I(ia2)}), Status::Ok);
}


// ---- GDI / user32 interop ------------------------------------------------------------------

TEST_P(Gdip_image_Test, HbitmapCompositesOverBackground) {
    // probe "hbitmap bg=ff0000ff ... 8080007f 000000ff ff00ff00 404040ff", "hbitmap_round".
    const u64 b = argb(2, 2, {0x80FF0000, 0x00000000, 0xFF00FF00, 0x40FFFFFF});
    auto dib_pixels = [&](u32 bg) {
        EXPECT_EQ(call("GdipCreateHBITMAPFromBitmap", {I(b), I(OUT), I(bg)}), Status::Ok);
        const Handle hbm = static_cast<Handle>(read_handle());
        EXPECT_EQ(gdi.get_object(hbm, 84, BUF, false), 84);  // a DIB section
        EXPECT_EQ(read_int(BUF + 32), 2);                    // biHeight > 0: bottom-up
        const GuestAddr bits = memory.read_u32(BUF + 20).value();
        Px out;
        for (i32 y = 1; y >= 0; --y)
            for (i32 x = 0; x < 2; ++x) out.push_back(memory.read_u32(bits + static_cast<u64>(y * 8 + x * 4)).value());
        return out;
    };
    EXPECT_EQ(dib_pixels(0xFF0000FF), (Px{0x8080007f, 0x000000ff, 0xff00ff00, 0x404040ff}));
    EXPECT_EQ(dib_pixels(0x000000FF), (Px{0x8080007f, 0x000000ff, 0xff00ff00, 0x404040ff}));
    EXPECT_EQ(dib_pixels(0xFFFFFFFF), (Px{0x80ff7f7f, 0x00ffffff, 0xff00ff00, 0x40ffffff}));
    const u64 r = argb(4, 1, {0x81808080, 0x7F7F7F7F, 0x01FFFFFF, 0xFE010203});
    ASSERT_EQ(call("GdipCreateHBITMAPFromBitmap", {I(r), I(OUT), I(0xFF406080)}), Status::Ok);
    ASSERT_EQ(gdi.get_object(static_cast<Handle>(read_handle()), 84, BUF, false), 84);
    const GuestAddr bits = memory.read_u32(BUF + 20).value();
    EXPECT_EQ(memory.read_u32(bits).value(), 0x81617080u);
    EXPECT_EQ(memory.read_u32(bits + 4).value(), 0x7f5f6f7fu);
    EXPECT_EQ(memory.read_u32(bits + 8).value(), 0x01416180u);
    EXPECT_EQ(memory.read_u32(bits + 12).value(), 0xfe010204u);
    EXPECT_EQ(call("GdipCreateHBITMAPFromBitmap", {I(b), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateHBITMAPFromBitmap", {I(0), I(OUT), I(0)}), Status::InvalidParameter);
    const u64 gray = bitmap(2, 2, FGray);
    EXPECT_EQ(call("GdipCreateHBITMAPFromBitmap", {I(gray), I(OUT), I(0)}), Status::InvalidParameter);
}

TEST_P(Gdip_image_Test, HiconRoundTrip) {
    // probe "hicon=0 fIcon=1 hot=1,1", " color ... 80800000 00000000 ff00ff00 40404040",
    // "mask ... c0004000", "fromhicon ... 00000000 00000000 ff00ff00 00000000", "hicon5x3 hot=2,1".
    const u64 b = argb(2, 2, {0x80FF0000, 0x00000000, 0xFF00FF00, 0x40FFFFFF});
    ASSERT_EQ(call("GdipCreateHICONFromBitmap", {I(b), I(OUT)}), Status::Ok);
    const u32 icon = static_cast<u32>(read_handle());
    const auto planes = resources.icon_planes(icon);
    ASSERT_TRUE(planes.has_value());
    EXPECT_TRUE(planes->is_icon);
    EXPECT_EQ(planes->hotspot_x, 1u);
    EXPECT_EQ(planes->hotspot_y, 1u);
    const std::vector<u8> color = {0x80, 0, 0, 0x80, 0, 0, 0, 0, 0, 0xff, 0, 0xff, 0x40, 0x40, 0x40, 0x40};
    EXPECT_EQ(planes->color, color);
    EXPECT_EQ(planes->mask, (std::vector<u8>{1, 1, 0, 1}));
    ASSERT_EQ(call("GdipCreateBitmapFromHICON", {I(icon), I(OUT)}), Status::Ok);
    const u64 back = read_handle();
    EXPECT_EQ(format(back), static_cast<u32>(FArgb));
    EXPECT_EQ(pixels(back), (Px{0, 0, 0xff00ff00, 0}));
    // The mask is set wherever alpha is not 255 (probe "hicon_mask6=f400").
    const u64 m6 = argb(6, 1, {0x7FFF0000, 0x80FF0000, 0x81FF0000, 0xFEFF0000, 0xFFFF0000, 0x01FF0000});
    ASSERT_EQ(call("GdipCreateHICONFromBitmap", {I(m6), I(OUT)}), Status::Ok);
    EXPECT_EQ(resources.icon_planes(static_cast<u32>(read_handle()))->mask, (std::vector<u8>{1, 1, 1, 1, 0, 1}));
    const u64 b53 = bitmap(5, 3);
    ASSERT_EQ(call("GdipCreateHICONFromBitmap", {I(b53), I(OUT)}), Status::Ok);
    EXPECT_EQ(resources.icon_planes(static_cast<u32>(read_handle()))->hotspot_x, 2u);
    // An alpha icon with an opaque mask reads back opaque (probe "fromhicon_alpha").
    IconPlanes p;
    p.width = 2;
    p.height = 2;
    p.color = {0x80, 0, 0, 0x80, 0, 0, 0, 0, 0, 0xff, 0, 0xff, 0x40, 0x40, 0x40, 0x40};
    p.mask = {0, 0, 0, 0};
    const u32 alpha_icon = resources.create_icon_object(p);
    ASSERT_NE(alpha_icon, 0u);
    ASSERT_EQ(call("GdipCreateBitmapFromHICON", {I(alpha_icon), I(OUT)}), Status::Ok);
    EXPECT_EQ(pixels(read_handle()), (Px{0xff800000, 0xff000000, 0xff00ff00, 0xff404040}));
    EXPECT_EQ(call("GdipCreateBitmapFromHICON", {I(0x1234), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromHICON", {I(alpha_icon), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateHICONFromBitmap", {I(b), I(0)}), Status::InvalidParameter);
}

TEST_P(Gdip_image_Test, BitmapFromGdiDibAliasesTheBits) {
    // probe "fromgdidib32 ... ff708090 ffa0b0c0 ff102030 ff405060", "gdidib_aliases",
    // "fromgdidib32_topdown", "fromgdidib565", "fromgdidib8", "gdidib8_pal count=2", "fromgdidib_rle=2".
    const GuestAddr bmi = BUF, bits = PIX;
    poison(bmi, 64, 0);
    write_ints(bmi, {40, 2, 2});
    ASSERT_TRUE(memory.write_u16(bmi + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(bmi + 14, 32).is_ok());
    write_ints(bits, {static_cast<i32>(0x80102030), 0x00405060, static_cast<i32>(0xFF708090), 0x01A0B0C0});
    ASSERT_EQ(call("GdipCreateBitmapFromGdiDib", {I(bmi), I(bits), I(OUT)}), Status::Ok);
    const u64 b = read_handle();
    EXPECT_EQ(format(b), static_cast<u32>(F32));
    EXPECT_EQ(pixels(b), (Px{0xff708090, 0xffa0b0c0, 0xff102030, 0xff405060}));
    ASSERT_TRUE(memory.write_u8(bits, 0x30 ^ 0xFF).is_ok());
    EXPECT_EQ(get_pixel(b, 0, 1), 0xff1020cfu);
    write_int(bmi + 8, -2);
    ASSERT_EQ(call("GdipCreateBitmapFromGdiDib", {I(bmi), I(bits), I(OUT)}), Status::Ok);
    EXPECT_EQ(pixels(read_handle()), (Px{0xff1020cf, 0xff405060, 0xff708090, 0xffa0b0c0}));
    write_int(bmi + 8, 2);
    write_int(bmi + 16, 2);  // BI_RLE8
    EXPECT_EQ(call("GdipCreateBitmapFromGdiDib", {I(bmi), I(bits), I(OUT)}), Status::InvalidParameter);
    write_int(bmi + 16, 0);
    EXPECT_EQ(call("GdipCreateBitmapFromGdiDib", {I(0), I(bits), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromGdiDib", {I(bmi), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateBitmapFromGdiDib", {I(bmi), I(bits), I(0)}), Status::InvalidParameter);
    // 16bpp BI_BITFIELDS 565 and BI_RGB 555.
    ASSERT_TRUE(memory.write_u16(bmi + 14, 16).is_ok());
    write_ints(bits, {0x03E07C00, 0x7FFF801F});
    ASSERT_EQ(call("GdipCreateBitmapFromGdiDib", {I(bmi), I(bits), I(OUT)}), Status::Ok);
    EXPECT_EQ(format(read_handle()), static_cast<u32>(F555));
    EXPECT_EQ(pixels(read_handle()), (Px{0xff0000ff, 0xffffffff, 0xffff0000, 0xff00ff00}));
    write_int(bmi + 16, 3);
    write_ints(bmi + 40, {0xF800, 0x07E0, 0x001F});
    ASSERT_EQ(call("GdipCreateBitmapFromGdiDib", {I(bmi), I(bits), I(OUT)}), Status::Ok);
    EXPECT_EQ(format(read_handle()), static_cast<u32>(F565));
    EXPECT_EQ(pixels(read_handle()), (Px{0xff8400ff, 0xff7bffff, 0xff7b8200, 0xff007d00}));
    // 8bpp with two colours used.
    poison(bmi, 64, 0);
    write_ints(bmi, {40, 2, 1});
    ASSERT_TRUE(memory.write_u16(bmi + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(bmi + 14, 8).is_ok());
    write_int(bmi + 32, 2);
    write_ints(bmi + 40, {0x00110000, 0x550000CC});
    write_ints(bits, {1});
    ASSERT_EQ(call("GdipCreateBitmapFromGdiDib", {I(bmi), I(bits), I(OUT)}), Status::Ok);
    const u64 b8 = read_handle();
    EXPECT_EQ(format(b8), static_cast<u32>(F8));
    EXPECT_EQ(palette_of(b8), (Px{0xff110000, 0xff0000cc}));
    EXPECT_EQ(pixels(b8), (Px{0xff0000cc, 0xff110000}));
}

TEST_P(Gdip_image_Test, BitmapFromHbitmap) {
    // probe "fromhbitmap_dib32=0 pf=00022009", "fromhbitmap_dib24 pf=00021808",
    // "hbitmap8_pal psize=1032", "hbitmap8_after_change" (a copy), "fromhbitmap_bad=7 null=7".
    const GuestAddr bmi = BUF, ppv = BUF + 0x200;
    poison(bmi, 64, 0);
    write_ints(bmi, {40, 2, -2});
    ASSERT_TRUE(memory.write_u16(bmi + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(bmi + 14, 32).is_ok());
    const Handle dib = gdi.create_dib_section(0, bmi, 0, ppv, x64());
    ASSERT_NE(dib, 0u);
    const GuestAddr bits = read_handle(ppv);
    write_ints(bits, {static_cast<i32>(0x80102030), 0x00405060, static_cast<i32>(0xFF708090), 0x01A0B0C0});
    ASSERT_EQ(call("GdipCreateBitmapFromHBITMAP", {I(dib), I(0), I(OUT)}), Status::Ok);
    const u64 b = read_handle();
    EXPECT_EQ(format(b), static_cast<u32>(F32));
    EXPECT_EQ(pixels(b), (Px{0xff102030, 0xff405060, 0xff708090, 0xffa0b0c0}));
    ASSERT_TRUE(memory.write_u32(bits, 0).is_ok());
    EXPECT_EQ(get_pixel(b, 0, 0), 0xff102030u);  // copied, not aliased
    poison(bmi, 64, 0);
    write_ints(bmi, {40, 2, -1});
    ASSERT_TRUE(memory.write_u16(bmi + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(bmi + 14, 8).is_ok());
    write_int(bmi + 32, 2);
    write_ints(bmi + 40, {0x00110000, 0x000000CC});
    const Handle dib8 = gdi.create_dib_section(0, bmi, 0, ppv, x64());
    ASSERT_NE(dib8, 0u);
    ASSERT_TRUE(memory.write_u8(read_handle(ppv), 1).is_ok());
    ASSERT_EQ(call("GdipCreateBitmapFromHBITMAP", {I(dib8), I(0), I(OUT)}), Status::Ok);
    const u64 b8 = read_handle();
    EXPECT_EQ(format(b8), static_cast<u32>(F8));
    ASSERT_EQ(call("GdipGetImagePaletteSize", {I(b8), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 1032);
    EXPECT_EQ(pixels(b8), (Px{0xff0000cc, 0xff110000}));
    EXPECT_EQ(call("GdipCreateBitmapFromHBITMAP", {I(0x1234), I(0), I(OUT)}), Status::Win32Error);
    EXPECT_EQ(call("GdipCreateBitmapFromHBITMAP", {I(0), I(0), I(OUT)}), Status::Win32Error);
}

// ---- ConvertFormat / palettes -----------------------------------------------------------------

const Px kConvSrc = {0xFF123456, 0x80FF4020, 0x00FFFFFF, 0xFFC0C0C0, 0xFF000000, 0xFFFFFFFF, 0xFF7F7F7F, 0xFF00FF80};

TEST_P(Gdip_image_Test, InitializePaletteFixedTypes) {
    // probe "== initpal".
    auto init = [&](u32 type, u32 colors, u32 transparent, u64 bmp, u32 cap = 256) {
        poison(PAL, 8 + 4 * 300, 0xEE);
        write_int(PAL + 4, static_cast<i32>(cap));
        return call("GdipInitializePalette", {I(PAL), I(type), I(colors), I(transparent), I(bmp)});
    };
    auto entry = [&](u32 i) { return memory.read_u32(PAL + 8 + 4 * i).value(); };
    auto count = [&] { return memory.read_u32(PAL + 4).value(); };
    auto flags = [&] { return memory.read_u32(PAL).value(); };
    ASSERT_EQ(init(0, 0, 0, 0), Status::Ok);
    EXPECT_EQ(flags(), 0xEEEEEEEEu);
    ASSERT_EQ(init(2, 0, 0, 0), Status::Ok);
    EXPECT_EQ(flags(), 0x200u);
    EXPECT_EQ(count(), 2u);
    EXPECT_EQ(entry(1), 0xffffffffu);
    ASSERT_EQ(init(2, 0, 1, 0), Status::Ok);
    EXPECT_EQ(count(), 3u);
    EXPECT_EQ(entry(2), 0u);
    ASSERT_EQ(init(3, 0, 0, 0), Status::Ok);
    EXPECT_EQ(count(), 16u);
    EXPECT_EQ(entry(1), 0xff0000ffu);
    EXPECT_EQ(entry(8), 0xffc0c0c0u);
    EXPECT_EQ(entry(15), 0xff008080u);
    ASSERT_EQ(init(4, 0, 0, 0), Status::Ok);
    EXPECT_EQ(count(), 35u);
    EXPECT_EQ(entry(1), 0xff00007fu);
    ASSERT_EQ(init(6, 0, 0, 0), Status::Ok);
    EXPECT_EQ(count(), 133u);
    EXPECT_EQ(entry(3), 0xff0000bfu);
    ASSERT_EQ(init(7, 0, 0, 0), Status::Ok);
    EXPECT_EQ(flags(), 0x700u);
    EXPECT_EQ(count(), 224u);
    EXPECT_EQ(entry(6), 0xff003300u);
    EXPECT_EQ(entry(223), 0xff008080u);
    ASSERT_EQ(init(8, 0, 0, 0), Status::Ok);
    EXPECT_EQ(count(), 252u);
    EXPECT_EQ(entry(6), 0xff002a00u);
    EXPECT_EQ(entry(251), 0xffffffffu);
    ASSERT_EQ(init(9, 0, 0, 0), Status::Ok);
    EXPECT_EQ(count(), 256u);
    EXPECT_EQ(entry(4), 0xff002400u);
    EXPECT_EQ(entry(12), 0xff006d00u);
    EXPECT_EQ(entry(255), 0xfffefeffu);
    ASSERT_EQ(init(9, 0, 1, 0), Status::Ok);
    EXPECT_EQ(count(), 256u);
    EXPECT_EQ(entry(255), 0u);
    ASSERT_EQ(init(10, 0, 0, 0), Status::Ok);
    EXPECT_EQ(flags(), 0xa00u);
    EXPECT_EQ(entry(1), 0xffffffffu);
    EXPECT_EQ(entry(5), 0xffff0000u);
    EXPECT_EQ(init(7, 0, 0, 0, 10), Status::GenericError);
    EXPECT_EQ(init(2, 0, 1, 0, 2), Status::GenericError);
    EXPECT_EQ(init(11, 0, 0, 0), Status::InvalidParameter);
    EXPECT_EQ(call("GdipInitializePalette", {I(0), I(2), I(0), I(0), I(0)}), Status::InvalidParameter);
    // Optimal: needs a bitmap and 2..256 colours; few colours come back exactly.
    const u64 b = argb(8, 1, kConvSrc);
    EXPECT_EQ(init(1, 4, 0, 0), Status::InvalidParameter);
    EXPECT_EQ(init(1, 1, 0, b), Status::InvalidParameter);
    EXPECT_EQ(init(1, 300, 0, b), Status::InvalidParameter);
    ASSERT_EQ(init(1, 256, 0, b), Status::Ok);
    EXPECT_EQ(count(), 7u);
    EXPECT_EQ(flags(), 0u);
    ASSERT_EQ(init(1, 4, 1, b), Status::Ok);
    EXPECT_EQ(count(), 4u);
    EXPECT_EQ(entry(3), 0u);
}

TEST_P(Gdip_image_Test, ConvertFormatRules) {
    // probe "== convert", "dither<d> tgt<t>", "convert_alpha<n>", "convert_fixed_nopal=2".
    auto pal216 = [&](u32 transparent) {
        write_int(PAL + 4, 256);
        EXPECT_EQ(call("GdipInitializePalette", {I(PAL), I(7), I(0), I(transparent), I(0)}), Status::Ok);
    };
    auto convert = [&](u64 b, i32 fmt, u32 dither, u32 pal_type, GuestAddr pal, float alpha) {
        return call("GdipBitmapConvertFormat", {I(b), I(static_cast<u32>(fmt)), I(dither), I(pal_type), I(pal), R(alpha)});
    };
    u64 b = argb(8, 1, kConvSrc);
    ASSERT_EQ(convert(b, F24, 0, 0, 0, 0), Status::Ok);
    EXPECT_EQ(format(b), static_cast<u32>(F24));
    EXPECT_EQ(pixels(b), (Px{0xff123456, 0xffff4020, 0xffffffff, 0xffc0c0c0, 0xff000000, 0xffffffff, 0xff7f7f7f, 0xff00ff80}));
    b = argb(8, 1, kConvSrc);
    ASSERT_EQ(convert(b, F565, 1, 0, 0, 0), Status::Ok);
    EXPECT_EQ(pixels(b), (Px{0xff103452, 0xffff4121, 0xffffffff, 0xffc6c3c6, 0xff000000, 0xffffffff, 0xff7b7d7b, 0xff00ff84}));
    b = argb(8, 1, kConvSrc);
    pal216(0);
    ASSERT_EQ(convert(b, F8, 1, 7, PAL, 50), Status::Ok);
    EXPECT_EQ(format(b), static_cast<u32>(F8));
    EXPECT_EQ(pixels(b), (Px{0xff003366, 0xffff3333, 0xff000000, 0xffcccccc, 0xff000000, 0xffffffff, 0xff808080, 0xff00ff99}));
    ASSERT_EQ(call("GdipGetImagePaletteSize", {I(b), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 8 + 4 * 224);
    const struct { float alpha; Px out; } thresholds[] = {
        {0, {0xff003366, 0xffff3333, 0xffffffff, 0xffcccccc, 0xff000000, 0xffffffff, 0xff808080, 0xff00ff99}},
        {50, {0xff003366, 0xffff3333, 0x00000000, 0xffcccccc, 0xff000000, 0xffffffff, 0xff808080, 0xff00ff99}},
        {100, {0xff003366, 0x00000000, 0x00000000, 0xffcccccc, 0xff000000, 0xffffffff, 0xff808080, 0xff00ff99}}};
    for (const auto& t : thresholds) {
        b = argb(8, 1, kConvSrc);
        pal216(1);
        ASSERT_EQ(convert(b, F8, 1, 7, PAL, t.alpha), Status::Ok);
        EXPECT_EQ(pixels(b), t.out) << t.alpha;
    }
    b = argb(8, 1, kConvSrc);
    write_int(PAL + 4, 256);
    ASSERT_EQ(call("GdipInitializePalette", {I(PAL), I(2), I(0), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(convert(b, F1, 0, 2, PAL, 0), Status::InvalidParameter);  // DitherNone onto FixedBW
    EXPECT_EQ(convert(b, F1, 3, 2, PAL, 0), Status::InvalidParameter);  // ordered onto FixedBW
    ASSERT_EQ(convert(b, F1, 1, 2, PAL, 0), Status::Ok);
    EXPECT_EQ(pixels(b), (Px{0xff000000, 0xff000000, 0xffffffff, 0xffffffff, 0xff000000, 0xffffffff, 0xff000000, 0xffffffff}));
    b = argb(8, 1, kConvSrc);
    pal216(0);
    EXPECT_EQ(convert(b, F8, 1, 7, 0, 0), Status::InvalidParameter);
    EXPECT_EQ(convert(b, F8, 2, 1, PAL, 0), Status::InvalidParameter);  // ordered with Optimal
    EXPECT_EQ(convert(b, F8, 10, 7, PAL, 0), Status::InvalidParameter);
    EXPECT_EQ(convert(b, FGray, 0, 0, 0, 0), Status::InvalidParameter);
    EXPECT_EQ(format(b), static_cast<u32>(FArgb));
    // Every dither type 0..9 maps onto a fixed halftone palette.
    for (u32 d = 0; d <= 9; ++d) {
        b = argb(8, 1, kConvSrc);
        pal216(0);
        EXPECT_EQ(convert(b, F8, d, 7, PAL, 0), Status::Ok) << d;
        EXPECT_EQ(get_pixel(b, 4, 0), 0xff000000u) << d;
        EXPECT_EQ(get_pixel(b, 5, 0), 0xffffffffu) << d;
    }
    // Back to 32bpp from indexed (probe "convert_8_to_32").
    ASSERT_EQ(convert(b, FArgb, 0, 0, 0, 0), Status::Ok);
    EXPECT_EQ(format(b), static_cast<u32>(FArgb));
}

TEST_P(Gdip_image_Test, HalftonePaletteIsANewGdiPalette) {
    // probe "halftone n=256", "halftone_same=0".
    const u64 h1 = call("GdipCreateHalftonePalette", {}) == Status::Ok ? 0 : cpu.gpr.rax;
    ASSERT_NE(h1, 0u);
    (void)call("GdipCreateHalftonePalette", {});
    EXPECT_NE(cpu.gpr.rax, h1);
    ASSERT_EQ(gdi.get_palette_entries(static_cast<Handle>(h1), 0, 256, BUF), 256u);
    EXPECT_EQ(memory.read_u32(BUF).value(), 0x00000000u);
    EXPECT_EQ(memory.read_u32(BUF + 4 * 7).value(), 0x00c0c0c0u);   // R,G,B,flags: c0 c0 c0 00
    EXPECT_EQ(memory.read_u32(BUF + 4 * 255).value(), 0x00ffffffu);
    EXPECT_EQ(memory.read_u8(BUF + 4 * 100 + 3).value(), 4u);        // PC_NOCOLLAPSE
}

// ---- DrawImage ---------------------------------------------------------------------------------------

class Gdip_image_draw_Test : public Gdip_image_Test {
protected:
    u64 target(i32 w, i32 h, i32 pom = 2) {
        dst = bitmap(w, h);
        EXPECT_EQ(call("GdipGetImageGraphicsContext", {I(dst), I(OUT)}), Status::Ok);
        g = read_handle(OUT);
        gr()->state.interpolation_mode = 5;
        gr()->state.pixel_offset_mode = pom;
        return g;
    }
    gdip::GpGraphics* gr() { return mgr.objects().get<gdip::GpGraphics>(g); }
    i32 covered() {
        i32 n = 0;
        for (u32 v : pixels(dst)) n += v != 0;
        return n;
    }
    void clear() {
        auto* img = mgr.objects().get<GpImage>(dst);
        std::fill(img->bits.begin(), img->bits.end(), 0);
    }
    u64 src4() { return argb(2, 2, {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF}); }
    u64 dst{0}, g{0};
};

TEST_P(Gdip_image_draw_Test, PhysicalSizeAndPoints) {
    // probe "drawI dpi96=0 covered=4", "dpi48 covered=16", "dpi192 covered=1", "dpi72 covered=9".
    target(8, 8);
    const u64 s = src4();
    for (auto [dpi, n] : {std::pair<float, i32>{96, 4}, {48, 16}, {192, 1}, {72, 9}}) {
        clear();
        ASSERT_EQ(call("GdipBitmapSetResolution", {I(s), R(dpi), R(dpi)}), Status::Ok);
        ASSERT_EQ(call("GdipDrawImageI", {I(g), I(s), I(1), I(1)}), Status::Ok);
        EXPECT_EQ(covered(), n) << dpi;
    }
    ASSERT_EQ(call("GdipBitmapSetResolution", {I(s), R(96), R(96)}), Status::Ok);
    clear();
    ASSERT_EQ(call("GdipDrawImageI", {I(g), I(s), I(1), I(1)}), Status::Ok);
    EXPECT_EQ(get_pixel(dst, 1, 1), 0xffff0000u);
    EXPECT_EQ(get_pixel(dst, 2, 2), 0xffffffffu);
    // probe "points3=0 p00=ff00ff00 p30=ffff0000 points4=6 points2=2 points0=2 null=2".
    write_reals(BUF, {4, 0, 0, 0, 4, 2, 0, 2});
    clear();
    ASSERT_EQ(call("GdipDrawImagePoints", {I(g), I(s), I(BUF), I(3)}), Status::Ok);
    EXPECT_EQ(get_pixel(dst, 0, 0), 0xff00ff00u);
    EXPECT_EQ(get_pixel(dst, 3, 0), 0xffff0000u);
    EXPECT_EQ(call("GdipDrawImagePoints", {I(g), I(s), I(BUF), I(4)}), Status::NotImplemented);
    EXPECT_EQ(call("GdipDrawImagePoints", {I(g), I(s), I(BUF), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawImagePoints", {I(g), I(s), I(BUF), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawImagePoints", {I(g), I(s), I(0), I(3)}), Status::InvalidParameter);
    write_ints(BUF + 0x100, {0, 0, 4, 0, 0, 4});
    EXPECT_EQ(call("GdipDrawImagePointsI", {I(g), I(s), I(BUF + 0x100), I(3)}), Status::Ok);
    // Source units (probe "pointsrect_unit_inch=0 pointsrect_world=2 pointsrect_display=2 pointsrect_mm=0").
    for (auto [unit, st] : {std::pair<u32, Status>{4, Status::Ok}, {0, Status::InvalidParameter},
                            {1, Status::InvalidParameter}, {6, Status::Ok}}) {
        EXPECT_EQ(call("GdipDrawImagePointsRect", {I(g), I(s), I(BUF), I(3), R(0), R(0), R(1), R(1), I(unit), I(0),
                                                   I(0), I(0)}),
                  st)
            << unit;
    }
    // probe "pointrect_dpi48_covered=16", "rectrect_mirror=0 ff00ff00 ffff0000".
    ASSERT_EQ(call("GdipBitmapSetResolution", {I(s), R(48), R(48)}), Status::Ok);
    clear();
    ASSERT_EQ(call("GdipDrawImagePointRectI", {I(g), I(s), I(0), I(0), I(0), I(0), I(2), I(2), I(2)}), Status::Ok);
    EXPECT_EQ(covered(), 16);
    ASSERT_EQ(call("GdipBitmapSetResolution", {I(s), R(96), R(96)}), Status::Ok);
    clear();
    ASSERT_EQ(call("GdipDrawImageRectRectI", {I(g), I(s), I(0), I(0), I(2), I(2), I(2), I(0), I(static_cast<u32>(-2)),
                                              I(2), I(2), I(0), I(0), I(0)}),
              Status::Ok);
    EXPECT_EQ(get_pixel(dst, 0, 0), 0xff00ff00u);
    EXPECT_EQ(get_pixel(dst, 1, 0), 0xffff0000u);
    EXPECT_EQ(call("GdipDrawImageRectRect", {I(g), I(s), R(0), R(0), R(4), R(4), R(0), R(0), R(0), R(2), I(2), I(0),
                                             I(0), I(0)}),
              Status::Ok);
}

TEST_P(Gdip_image_draw_Test, PixelOffsetModesAndScaling) {
    // probe "pom<n> 1:1" (exact in every mode) and "| 2x:" (None/HighSpeed
    // show GDI+'s half-pixel shift; Half/HighQuality are exact).
    const u64 s = src4();
    const Px exact2x = {0xff0000, 0xff0000, 0x00ff00, 0x00ff00, 0xff0000, 0xff0000, 0x00ff00, 0x00ff00,
                        0x0000ff, 0x0000ff, 0xffffff, 0xffffff, 0x0000ff, 0x0000ff, 0xffffff, 0xffffff};
    const Px shifted2x = {0xff0000, 0x00ff00, 0x00ff00, 0, 0x0000ff, 0xffffff, 0xffffff, 0,
                          0x0000ff, 0xffffff, 0xffffff, 0, 0, 0, 0, 0};
    for (i32 pom = 0; pom <= 4; ++pom) {
        target(4, 4, pom);
        ASSERT_EQ(call("GdipDrawImageI", {I(g), I(s), I(1), I(1)}), Status::Ok);
        Px p = pixels(dst);
        EXPECT_EQ(p[5] & 0xFFFFFF, 0xff0000u) << pom;
        EXPECT_EQ(p[10] & 0xFFFFFF, 0xffffffu) << pom;
        EXPECT_EQ(p[0], 0u) << pom;
        EXPECT_EQ(p[15], 0u) << pom;
        clear();
        ASSERT_EQ(call("GdipDrawImageRectI", {I(g), I(s), I(0), I(0), I(4), I(4)}), Status::Ok);
        p = pixels(dst);
        for (u32& v : p) v &= 0xFFFFFF;
        EXPECT_EQ(p, (pom == 2 || pom == 4) ? exact2x : shifted2x) << pom;
    }
    // A world scale applies to the physical size (probe "world2x"); a page
    // unit of inches keeps it physical (probe "pageinch_draw=0 covered=4").
    target(8, 8);
    gr()->state.world = gdip::Affine{2, 0, 0, 2, 0, 0};
    ASSERT_EQ(call("GdipDrawImageI", {I(g), I(s), I(1), I(1)}), Status::Ok);
    EXPECT_EQ(covered(), 16);
    EXPECT_EQ(get_pixel(dst, 2, 2), 0xffff0000u);
    EXPECT_EQ(get_pixel(dst, 5, 5), 0xffffffffu);
    target(8, 8);
    gr()->state.page_unit = gdip::UnitInch;
    ASSERT_EQ(call("GdipDrawImage", {I(g), I(s), R(0), R(0)}), Status::Ok);
    EXPECT_EQ(covered(), 4);
}

TEST_P(Gdip_image_draw_Test, AbortLockAndSourceRules) {
    target(8, 8);
    const u64 s = src4();
    // probe "abort_false=0 calls=1 abort_true=9 calls=1 drawn=00000000".
    int calls = 0;
    gdi.set_guest_call([&calls](GuestAddr fn, const u64* args, usize n, bool) -> u64 {
        EXPECT_EQ(fn, 0x7000u);
        EXPECT_EQ(n, 1u);
        ++calls;
        return args[0] != 0 ? 1 : 0;
    });
    ASSERT_EQ(call("GdipDrawImageRectRectI", {I(g), I(s), I(0), I(0), I(4), I(4), I(0), I(0), I(2), I(2), I(2), I(0),
                                              I(0x7000), I(0)}),
              Status::Ok);
    EXPECT_EQ(calls, 1);
    clear();
    EXPECT_EQ(call("GdipDrawImageRectRectI", {I(g), I(s), I(0), I(0), I(4), I(4), I(0), I(0), I(2), I(2), I(2), I(0),
                                              I(0x7000), I(1)}),
              Status::Aborted);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(covered(), 0);
    // GdipImageSetAbort: E_ABORT aborts, another failure is GenericError,
    // success codes draw (probe "imageabort ret=...").
    const GuestAddr obj = BUF, vtbl = BUF + 0x10;
    ASSERT_TRUE(memory.write_u64(obj, vtbl).is_ok());
    ASSERT_TRUE(memory.write_u64(vtbl, 0x7100).is_ok());
    u32 hr = 0;
    gdi.set_guest_call([&](GuestAddr fn, const u64* args, usize, bool) -> u64 {
        EXPECT_EQ(fn, 0x7100u);
        EXPECT_EQ(args[0], obj);
        return hr;
    });
    ASSERT_EQ(call("GdipImageSetAbort", {I(s), I(obj)}), Status::Ok);
    for (auto [code, st] : {std::pair<u32, Status>{0, Status::Ok}, {1, Status::Ok}, {0x80004004u, Status::Aborted},
                            {0x80004005u, Status::GenericError}}) {
        hr = code;
        EXPECT_EQ(call("GdipDrawImageI", {I(g), I(s), I(0), I(0)}), st) << std::hex << code;
    }
    ASSERT_EQ(call("GdipImageSetAbort", {I(s), I(0)}), Status::Ok);
    // probe "draw_locked=8 draw_self=0 draw_null_img=2 draw_null_g=2", "draw_gray=2".
    ASSERT_EQ(lock(s, 1, FArgb), Status::Ok);
    EXPECT_EQ(call("GdipDrawImageI", {I(g), I(s), I(0), I(0)}), Status::WrongState);
    ASSERT_EQ(unlock(s), Status::Ok);
    EXPECT_EQ(call("GdipDrawImageI", {I(g), I(dst), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipDrawImageI", {I(g), I(0), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawImageI", {I(0), I(s), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawImageI", {I(g), I(bitmap(2, 2, FGray)), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawImageI", {I(g), I(bitmap(2, 2, FCmyk)), I(0), I(0)}), Status::InvalidParameter);
    // Source-over (probe "srcover=ff80007f srcover_on_clear=80ff0000").
    const u64 t = argb(1, 1, {0x80FF0000});
    target(1, 1);
    ASSERT_EQ(set_pixel(dst, 0, 0, 0xFF0000FF), Status::Ok);
    ASSERT_EQ(call("GdipDrawImageI", {I(g), I(t), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(get_pixel(dst, 0, 0), 0xff80007fu);
    target(1, 1);
    ASSERT_EQ(call("GdipDrawImageI", {I(g), I(t), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(get_pixel(dst, 0, 0), 0x80ff0000u);
}

TEST_P(Gdip_image_draw_Test, DrawImageFXAndCachedBitmaps) {
    // probe "fx_null=0 fx_src=0 fx_unit_inch=0", "fx_matrix=0 covered=16 minx=1".
    target(8, 8);
    const u64 s = src4();
    EXPECT_EQ(call("GdipDrawImageFX", {I(g), I(s), I(0), I(0), I(0), I(0), I(2)}), Status::Ok);
    write_reals(RECT, {0, 0, 2, 2});
    EXPECT_EQ(call("GdipDrawImageFX", {I(g), I(s), I(RECT), I(0), I(0), I(0), I(4)}), Status::Ok);
    const u64 mat = mgr.objects().add(gdip::GpMatrix{gdip::Affine{2, 0, 0, 2, 1, 1}});
    clear();
    ASSERT_EQ(call("GdipDrawImageFX", {I(g), I(s), I(0), I(mat), I(0), I(0), I(2)}), Status::Ok);
    EXPECT_EQ(covered(), 16);
    EXPECT_EQ(get_pixel(dst, 0, 1), 0u);
    EXPECT_EQ(get_pixel(dst, 1, 1), 0xffff0000u);
    // probe "cached_create=0 draw=0 px33=ffff0000 px44=ffffffff draw_translated=0 px43=ffff0000
    // draw_rotated=8 draw_scaled=8 null_create=2,2,2 delete_null=2 draw_null=2".
    ASSERT_EQ(call("GdipCreateCachedBitmap", {I(s), I(g), I(OUT2)}), Status::Ok);
    const u64 cb = read_handle(OUT2);
    clear();
    ASSERT_EQ(call("GdipDrawCachedBitmap", {I(g), I(cb), I(3), I(3)}), Status::Ok);
    EXPECT_EQ(get_pixel(dst, 3, 3), 0xffff0000u);
    EXPECT_EQ(get_pixel(dst, 4, 4), 0xffffffffu);
    EXPECT_EQ(covered(), 4);
    for (i32 pom : {0, 2}) {
        gr()->state.pixel_offset_mode = pom;
        gr()->state.world = gdip::Affine{1, 0, 0, 1, 1, 0};
        clear();
        ASSERT_EQ(call("GdipDrawCachedBitmap", {I(g), I(cb), I(3), I(3)}), Status::Ok);
        EXPECT_EQ(get_pixel(dst, 4, 3), 0xffff0000u) << pom;
        EXPECT_EQ(covered(), 4) << pom;
    }
    gr()->state.world = gdip::Affine{0.866f, 0.5f, -0.5f, 0.866f, 0, 0};
    EXPECT_EQ(call("GdipDrawCachedBitmap", {I(g), I(cb), I(3), I(3)}), Status::WrongState);
    gr()->state.world = gdip::Affine{2, 0, 0, 2, 0, 0};
    EXPECT_EQ(call("GdipDrawCachedBitmap", {I(g), I(cb), I(3), I(3)}), Status::WrongState);
    EXPECT_EQ(call("GdipCreateCachedBitmap", {I(0), I(g), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateCachedBitmap", {I(s), I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateCachedBitmap", {I(s), I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawCachedBitmap", {I(g), I(0), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteCachedBitmap", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteCachedBitmap", {I(cb)}), Status::Ok);
}

TEST_P(Gdip_image_draw_Test, IndexedTargetsDraw) {
    // probe "indexed_graphics=0 fill=0 clear=0 drawimage=0": an 8bpp image
    // takes drawing, quantised to its palette.
    const u64 b8 = bitmap(2, 1, F8);
    ASSERT_EQ(call("GdipGetImageGraphicsContext", {I(b8), I(OUT)}), Status::Ok);
    g = read_handle(OUT);
    const u64 s = argb(2, 1, {0xFFFF0000, 0xFF123456});
    ASSERT_EQ(call("GdipDrawImageI", {I(g), I(s), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(pixels(b8), (Px{0xffff0000, 0xff003366}));
}

TEST_P(Gdip_image_draw_Test, WrongClassHandlesAreObjectBusy) {
    // probe "== wrongclass": a live object of another class is ObjectBusy,
    // except a non-graphics as the graphics of a draw (InvalidParameter).
    target(2, 2);
    const u64 b = dst;
    const u64 ia = attributes();
    EXPECT_EQ(call("GdipGetImageWidth", {I(g), I(OUT3)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipBitmapGetPixel", {I(g), I(0), I(0), I(OUT3)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipBitmapSetPixel", {I(ia), I(0), I(0), I(0)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipBitmapLockBits", {I(g), I(0), I(1), I(FArgb), I(BD)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipCloneImage", {I(g), I(OUT)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipCloneBitmapAreaI", {I(0), I(0), I(1), I(1), I(FArgb), I(g), I(OUT)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipImageRotateFlip", {I(g), I(0)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipGetImageGraphicsContext", {I(ia), I(OUT)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipDrawImageI", {I(g), I(ia), I(0), I(0)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipDrawImageI", {I(b), I(b), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawImageRectRectI", {I(g), I(b), I(0), I(0), I(1), I(1), I(0), I(0), I(1), I(1), I(2), I(b),
                                              I(0), I(0)}),
              Status::ObjectBusy);
    EXPECT_EQ(call("GdipSetImageAttributesGamma", {I(b), I(0), I(1), R(1.0f)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipDisposeImageAttributes", {I(b)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipCloneImageAttributes", {I(b), I(OUT)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipCreateHBITMAPFromBitmap", {I(ia), I(OUT), I(0)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipCreateCachedBitmap", {I(ia), I(g), I(OUT)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipDisposeImage", {I(ia)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipDisposeImage", {I(0x1234)}), Status::InvalidParameter);
}

INSTANTIATE_GDIP_BITNESS(Gdip_image_draw_Test);

INSTANTIATE_GDIP_BITNESS(Gdip_image_Test);

}  // namespace
}  // namespace vitreo::winapi::gdip_test
