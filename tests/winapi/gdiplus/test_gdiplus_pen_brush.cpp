// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus_pen_brush.cpp
/// @brief Contract tests for the GDI+ brush area (gdiplus_pen_brush.cpp), run in
/// both bitnesses through gdiplus_fixture.hpp.
///
/// Expected values are Windows' own: the winoracle capture
/// tools/oracle_probe/gdiplus_brush_probe.expected (x64) / .x86.expected
/// (x86), read at run time where a test compares a rendered row, and quoted
/// from it otherwise.

#include "gdiplus_fixture.hpp"

#include <winapi/graphics/gdiplus/gdiplus_geometry.hpp>
#include <winapi/graphics/gdiplus/gdiplus_graphics.hpp>
#include <winapi/graphics/gdiplus/gdiplus_image.hpp>
#include <winapi/graphics/gdiplus/gdiplus_pen_brush.hpp>
#include <winapi/graphics/gdiplus/gdiplus_render.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>

namespace vitreo::winapi::gdip_test {
namespace {

using gdip::Affine;
using gdip::GpBrush;
using gdip::GpGraphics;
using gdip::GpImage;
using gdip::GpMatrix;
using gdip::GpPath;
using gdip::RectF;

class Gdip_pen_brush_Test : public GdipTest {
protected:
    // ---- Oracle capture ----------------------------------------------------
    std::string probe_line(const std::string& prefix) {
        const std::string file = std::string(VITREO_ORACLE_PROBE_DIR) +
                                 (x64() ? "/gdiplus_brush_probe.expected" : "/gdiplus_brush_probe.x86.expected");
        std::ifstream in(file);
        EXPECT_TRUE(in.good()) << file;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
        }
        ADD_FAILURE() << "probe row not found: " << prefix;
        return {};
    }
    /// The ARGB values of a "<tag> y=N:" / "x=N:" row.
    std::vector<u32> probe_row(const std::string& prefix) {
        std::istringstream s(probe_line(prefix));
        std::vector<u32> out;
        std::string tok;
        while (s >> tok) out.push_back(static_cast<u32>(std::strtoul(tok.c_str(), nullptr, 16)));
        return out;
    }

    // ---- Objects -----------------------------------------------------------
    u64 make_brush(const std::string& fn, std::initializer_list<Arg> args) {
        EXPECT_EQ(call(fn, args), Status::Ok) << fn;
        return read_handle(OUT);
    }
    GpBrush* brush(u64 h) { return mgr.objects().get<GpBrush>(h); }
    u64 matrix(Affine a) { return mgr.objects().add(GpMatrix{a}); }
    Affine matrix_of(u64 h) { return mgr.objects().get<GpMatrix>(h)->m; }
    u64 bitmap(i32 w, i32 h, i32 format = gdip::PixelFormat32bppARGB) {
        GpImage img;
        img.width = w;
        img.height = h;
        img.pixel_format = format;
        img.stride = w * (format == gdip::PixelFormat24bppRGB ? 3 : 4);
        img.bits.assign(static_cast<usize>(img.stride) * static_cast<usize>(h), 0);
        return mgr.objects().add(std::move(img));
    }
    void set_px(u64 img, i32 x, i32 y, u32 argb) {
        GpImage* i = mgr.objects().get<GpImage>(img);
        u8* p = &i->bits[static_cast<usize>(y) * static_cast<usize>(i->stride) + static_cast<usize>(x) * 4u];
        p[0] = static_cast<u8>(argb);
        p[1] = static_cast<u8>(argb >> 8);
        p[2] = static_cast<u8>(argb >> 16);
        p[3] = static_cast<u8>(argb >> 24);
    }
    u32 px(u64 img, i32 x, i32 y) {
        GpImage* i = mgr.objects().get<GpImage>(img);
        const u8* p = &i->bits[static_cast<usize>(y) * static_cast<usize>(i->stride) + static_cast<usize>(x) * 4u];
        return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
               static_cast<u32>(p[3]) << 24;
    }
    /// The probe's 4x2 texture: pixel (x, y) = ff, x*0x40, y*0x80, 0x11.
    u64 probe_texture_image() {
        const u64 img = bitmap(4, 2);
        for (i32 y = 0; y < 2; ++y)
            for (i32 x = 0; x < 4; ++x)
                set_px(img, x, y, 0xFF000000u | static_cast<u32>(x * 0x40) << 16 | static_cast<u32>(y * 0x80) << 8 | 0x11);
        return img;
    }

    /// Fill with @p brush_h on a fresh transparent ARGB canvas with
    /// SourceCopy compositing, as the probe's canvas() does. The probe
    /// filled @p r covering the sampled pixels; the brush colours do not
    /// depend on the shape, so (unless exact_rect) a shape well beyond the
    /// canvas is filled and edge coverage — the draw area's contract —
    /// stays out of these tests.
    bool exact_rect = false;
    u64 fill(u64 brush_h, i32 w, i32 h, RectF r, std::function<void(gdip::GraphicsState&)> state = {}) {
        if (!exact_rect) r = {-64, -64, static_cast<float>(w) + 128, static_cast<float>(h) + 128};
        const u64 img = bitmap(w, h);
        GpGraphics g;
        g.kind = gdip::TargetKind::Image;
        g.image = img;
        g.state.compositing_mode = 1;
        if (state) state(g.state);
        gdip::RenderScope scope(mgr, marshaler, g);
        EXPECT_EQ(scope.status(), Status::Ok);
        CGMutablePathRef path = CGPathCreateMutable();
        CGPathAddRect(path, nullptr, CGRectMake(r.X, r.Y, r.Width, r.Height));
        EXPECT_EQ(gdip::fill_path(scope, path, false, *brush(brush_h)), Status::Ok);
        CGPathRelease(path);
        EXPECT_EQ(scope.commit(), Status::Ok);
        return img;
    }
    /// Compare a row (or column) of @p img with the probe row, each channel
    /// within @p tol.
    void expect_row(u64 img, const std::string& prefix, i32 fixed, bool column, i32 tol, i32 from = 0,
                    i32 count = -1) {
        const std::vector<u32> want = probe_row(prefix);
        ASSERT_FALSE(want.empty()) << prefix;
        const i32 n = count < 0 ? static_cast<i32>(want.size()) - from : count;
        for (i32 i = from; i < from + n; ++i) {
            const u32 got = column ? px(img, fixed, i) : px(img, i, fixed);
            const u32 exp = want[static_cast<usize>(i)];
            for (int c = 0; c < 32; c += 8) {
                const i32 d = std::abs(static_cast<i32>((got >> c) & 0xFF) - static_cast<i32>((exp >> c) & 0xFF));
                EXPECT_LE(d, tol) << prefix << " at " << i << ": got " << std::hex << got << " want " << exp;
                if (d > tol) break;
            }
        }
    }
    void expect_matrix(const Affine& a, const std::vector<float>& e, float tol = 1e-4f) {
        const float v[6] = {a.m11, a.m12, a.m21, a.m22, a.dx, a.dy};
        usize i = 0;
        for (float x : e) EXPECT_NEAR(v[i++], x, tol) << "element " << (i - 1);
    }
    u64 point_buf(GuestAddr at, std::initializer_list<float> xy) {
        write_reals(at, xy);
        return at;
    }
};

// ---- Brush / SolidFill ----------------------------------------------------------

TEST_P(Gdip_pen_brush_Test, SolidFillCreateGetSetCloneDelete) {
    EXPECT_EQ(call("GdipCreateSolidFill", {I(0xff112233), I(0)}), Status::InvalidParameter);
    const u64 b = make_brush("GdipCreateSolidFill", {I(0x80112233), I(OUT)});
    EXPECT_EQ(call("GdipGetSolidFillColor", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0x80112233u);
    EXPECT_EQ(call("GdipGetBrushType", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("GdipGetSolidFillColor", {I(b), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetSolidFillColor", {I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetSolidFillColor", {I(b), I(0x01020304)}), Status::Ok);
    EXPECT_EQ(call("GdipSetSolidFillColor", {I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetBrushType", {I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetBrushType", {I(b), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCloneBrush", {I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCloneBrush", {I(b), I(0)}), Status::InvalidParameter);
    poison(OUT3);
    EXPECT_EQ(call("GdipCloneBrush", {I(b), I(OUT3)}), Status::Ok);
    const u64 c = read_handle(OUT3);
    EXPECT_NE(c, b);
    EXPECT_EQ(call("GdipSetSolidFillColor", {I(b), I(0xffffffff)}), Status::Ok);
    EXPECT_EQ(call("GdipGetSolidFillColor", {I(c), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0x01020304u);  // "clone ... color 01020304"
    EXPECT_EQ(call("GdipDeleteBrush", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteBrush", {I(c)}), Status::Ok);
    EXPECT_EQ(call("GdipDeleteBrush", {I(c)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteBrush", {I(b)}), Status::Ok);
}

TEST_P(Gdip_pen_brush_Test, SolidFillPaints) {
    const u64 b = make_brush("GdipCreateSolidFill", {I(0x80ff0000), I(OUT)});
    exact_rect = true;
    const u64 img = fill(b, 6, 6, {2, 2, 2, 2});
    exact_rect = false;
    EXPECT_EQ(px(img, 0, 0), 0u);
    EXPECT_EQ(px(img, 3, 3), 0x80ff0000u);
    EXPECT_EQ(px(img, 5, 5), 0u);
    u32 argb = 0;
    EXPECT_TRUE(gdip::brush_solid_argb(*brush(b), argb));
    EXPECT_EQ(argb, 0x80ff0000u);
}

TEST_P(Gdip_pen_brush_Test, WrongBrushTypeIsRefused) {
    const u64 solid = make_brush("GdipCreateSolidFill", {I(0xff000000), I(OUT)});
    const u64 hatch = make_brush("GdipCreateHatchBrush", {I(0), I(0xff000000), I(0xffffffff), I(OUT)});
    EXPECT_EQ(call("GdipGetSolidFillColor", {I(hatch), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetHatchStyle", {I(solid), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetLineColors", {I(solid), I(OUT2)}), Status::InvalidParameter);
    write_reals(BUF, {0, 0, 10, 0, 0, 10});
    EXPECT_EQ(call("GdipGetTextureImage", {I(solid), I(OUT2)}), Status::InvalidParameter);
    // A live object of another class: ObjectBusy, as for every GDI+ object
    // argument (draw area probe); an unknown handle: InvalidParameter.
    const u64 mx = matrix({});
    EXPECT_EQ(call("GdipDeleteBrush", {I(mx)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipGetSolidFillColor", {I(mx), I(OUT2)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipGetLineTransform", {I(mx), I(mx)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipDeleteBrush", {I(0x12345670)}), Status::InvalidParameter);
    const u64 lb = make_brush("GdipCreatePathGradient", {I(BUF), I(3), I(0), I(OUT)});
    EXPECT_EQ(call("GdipGetPathGradientTransform", {I(lb), I(solid)}), Status::ObjectBusy);
}

// ---- HatchBrush ----------------------------------------------------------------

TEST_P(Gdip_pen_brush_Test, HatchCreateAndGetters) {
    EXPECT_EQ(call("GdipCreateHatchBrush", {I(static_cast<u64>(-1) & 0xffffffff), I(0), I(0), I(OUT)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateHatchBrush", {I(53), I(0), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateHatchBrush", {I(0), I(0), I(0), I(0)}), Status::InvalidParameter);
    const u64 b = make_brush("GdipCreateHatchBrush", {I(52), I(0x80102030), I(0x40506070), I(OUT)});
    EXPECT_EQ(call("GdipGetHatchStyle", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 52);
    EXPECT_EQ(call("GdipGetHatchForegroundColor", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0x80102030u);
    EXPECT_EQ(call("GdipGetHatchBackgroundColor", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0x40506070u);
    EXPECT_EQ(call("GdipGetBrushType", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
    for (const char* fn : {"GdipGetHatchStyle", "GdipGetHatchForegroundColor", "GdipGetHatchBackgroundColor"}) {
        EXPECT_EQ(call(fn, {I(b), I(0)}), Status::InvalidParameter) << fn;
        EXPECT_EQ(call(fn, {I(0), I(OUT2)}), Status::InvalidParameter) << fn;
    }
}

// Vitreo draws its own hatch cells from the Learn HatchStyle descriptions;
// these tests check the documented properties, not Windows' bitmaps.
TEST_P(Gdip_pen_brush_Test, HatchCellsHaveTheDocumentedProperties) {
    auto cell = [&](i32 style) {
        const u64 b = make_brush("GdipCreateHatchBrush", {I(static_cast<u64>(style)), I(0xff000000), I(0xffffffff), I(OUT)});
        const u64 img = fill(b, 16, 16, {0, 0, 16, 16});
        // Period 8 (probe "hatchperiod N periodic=1" for every style).
        EXPECT_EQ(probe_line("hatchperiod " + std::to_string(style) + " "), "periodic=1");
        std::vector<i32> fg(64);  // foreground weight 0..255 (black on white)
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                const u32 v = px(img, x, y);
                EXPECT_EQ(v, px(img, x & 7, y & 7)) << "style " << style << " not 8-periodic";
                EXPECT_EQ(v >> 24, 0xffu);
                if (x < 8 && y < 8) fg[static_cast<usize>(y * 8 + x)] = 255 - static_cast<i32>(v & 0xff);
            }
        return fg;
    };
    auto count = [](const std::vector<i32>& c) {
        i32 n = 0;
        for (i32 v : c) n += v == 255;
        return n;
    };
    for (i32 style = 0; style < 53; ++style) {
        const auto c = cell(style);
        const i32 n = count(c);
        EXPECT_GT(n, 0) << "style " << style << " has no foreground";
        EXPECT_LT(n, 64) << "style " << style << " has no background";
        // Only the antialiased styles (Learn: Forward/BackwardDiagonal,
        // DiagonalCross) carry partial pixels.
        if (style == 2 || style == 3 || style == 5) continue;
        for (i32 v : c) EXPECT_TRUE(v == 0 || v == 255) << "style " << style;
    }
    // Horizontal / Vertical: uniform rows / columns.
    const auto h = cell(0), v = cell(1);
    for (i32 y = 0; y < 8; ++y)
        for (i32 x = 1; x < 8; ++x) {
            EXPECT_EQ(h[static_cast<usize>(y * 8 + x)], h[static_cast<usize>(y * 8)]);
            EXPECT_EQ(v[static_cast<usize>(x * 8 + y)], v[static_cast<usize>(y)]);
        }
    // Percent styles: foreground ratio within one pixel of the stated percent.
    const i32 pct[12] = {5, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90};
    for (i32 i = 0; i < 12; ++i) EXPECT_NEAR(count(cell(6 + i)), pct[i] * 64 / 100.0, 1.0) << pct[i] << "%";
    // Relative spacing / width as Learn describes them.
    EXPECT_EQ(count(cell(25)), 2 * count(cell(0)));  // LightHorizontal: half the spacing
    EXPECT_EQ(count(cell(27)), 4 * count(cell(0)));  // NarrowHorizontal: a quarter
    EXPECT_EQ(count(cell(28)), 4 * count(cell(1)));  // DarkVertical: half spacing, twice the width
    EXPECT_EQ(count(cell(18)), 2 * 8);               // LightDownward: two diagonals per cell
    EXPECT_EQ(count(cell(22)), 3 * 8);               // WideDownward: one diagonal, triple width
    // Forward diagonal slants right going down; backward slants left.
    const auto f = cell(2), bk = cell(3);
    for (i32 i = 0; i < 64; ++i) {
        const i32 x = i % 8, y = i / 8;
        EXPECT_EQ(f[static_cast<usize>(i)], f[static_cast<usize>(((y + 1) & 7) * 8 + ((x + 1) & 7))]);
        EXPECT_EQ(bk[static_cast<usize>(i)], bk[static_cast<usize>(((y + 1) & 7) * 8 + ((x + 7) & 7))]);
    }
    // Checkerboards: the large one has squares twice the size.
    const auto sc = cell(49), lc = cell(50);
    EXPECT_EQ(count(sc), 32);
    EXPECT_EQ(count(lc), 32);
    EXPECT_NE(sc[0], sc[2]);
    EXPECT_EQ(lc[0], lc[3]);
    EXPECT_NE(lc[0], lc[4]);
}

TEST_P(Gdip_pen_brush_Test, HatchIsAnchoredToDevicePixelsAndRenderingOrigin) {
    for (i32 style : {0, 13, 26, 39, 52}) {
        EXPECT_EQ(probe_line("hatchanchor " + std::to_string(style) + " "),
                  "world-translate-ignored=1 world-scale-ignored=1 origin-shifts-cell=1");
        const u64 b = make_brush("GdipCreateHatchBrush", {I(static_cast<u64>(style)), I(0xff000000), I(0xffffffff), I(OUT)});
        const u64 base = fill(b, 16, 16, {});
        const u64 moved = fill(b, 16, 16, {}, [](gdip::GraphicsState& s) { s.world.dx = 1; s.world.dy = 2; });
        const u64 scaled = fill(b, 16, 16, {}, [](gdip::GraphicsState& s) { s.world.m11 = 2; s.world.m22 = 2; });
        const u64 origin = fill(b, 16, 16, {}, [](gdip::GraphicsState& s) {
            s.rendering_origin_x = 3;
            s.rendering_origin_y = 1;
        });
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                EXPECT_EQ(px(moved, x, y), px(base, x, y));
                EXPECT_EQ(px(scaled, x, y), px(base, x, y));
                EXPECT_EQ(px(origin, x, y), px(base, (x - 3) & 7, (y - 1) & 7));
            }
    }
    // Fully covered pixels carry the colours as given ("hatch alpha copy").
    const u64 a = make_brush("GdipCreateHatchBrush", {I(0), I(0x80ff0000), I(0x400000ff), I(OUT)});
    const u64 ai = fill(a, 16, 16, {});
    char want[64];
    std::snprintf(want, sizeof want, "fg=%08x bg=%08x", px(ai, 0, 0), px(ai, 0, 1));
    EXPECT_EQ(probe_line("hatch alpha copy "), want);
}

// ---- TextureBrush ----------------------------------------------------------------

TEST_P(Gdip_pen_brush_Test, TextureCreateRules) {
    const u64 img = probe_texture_image();
    // "texture wrap -1 st=3" ... "wrap 5 st=3": an unknown wrap is OutOfMemory.
    EXPECT_EQ(call("GdipCreateTexture", {I(img), I(0xffffffff), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreateTexture", {I(img), I(5), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreateTexture", {I(0), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateTexture", {I(img), I(0), I(0)}), Status::InvalidParameter);
    for (i32 w = 0; w <= 4; ++w) {
        const u64 b = make_brush("GdipCreateTexture", {I(img), I(static_cast<u64>(w)), I(OUT)});
        EXPECT_EQ(call("GdipGetTextureWrapMode", {I(b), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), w);
    }
    const u64 b = make_brush("GdipCreateTexture", {I(img), I(0), I(OUT)});
    EXPECT_EQ(call("GdipGetBrushType", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 2);
    poison(OUT2);
    EXPECT_EQ(call("GdipGetTextureImage", {I(b), I(OUT2)}), Status::Ok);
    const u64 ti = read_handle(OUT2);
    EXPECT_EQ(call("GdipGetTextureImage", {I(b), I(OUT3)}), Status::Ok);
    EXPECT_NE(read_handle(OUT3), ti);  // "texture image twice distinct=1"
    const GpImage* t = mgr.objects().get<GpImage>(ti);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->width, 4);
    EXPECT_EQ(t->height, 2);
    EXPECT_EQ(t->pixel_format, 0xe200b);  // "pf=e200b"
    EXPECT_EQ(call("GdipGetTextureImage", {I(b), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetTextureImage", {I(0), I(OUT2)}), Status::InvalidParameter);
    // A 24bpp source is reported as PARGB too ("texture from 24bpp image pf=e200b").
    const u64 b24 = make_brush("GdipCreateTexture", {I(bitmap(3, 3, gdip::PixelFormat24bppRGB)), I(0), I(OUT)});
    EXPECT_EQ(call("GdipGetTextureImage", {I(b24), I(OUT2)}), Status::Ok);
    EXPECT_EQ(mgr.objects().get<GpImage>(read_handle(OUT2))->pixel_format, 0xe200b);
    // Wrap setter: out-of-range values are accepted and ignored.
    EXPECT_EQ(call("GdipSetTextureWrapMode", {I(b), I(4)}), Status::Ok);
    EXPECT_EQ(call("GdipSetTextureWrapMode", {I(b), I(5)}), Status::Ok);
    EXPECT_EQ(call("GdipSetTextureWrapMode", {I(b), I(0xffffffff)}), Status::Ok);
    EXPECT_EQ(call("GdipGetTextureWrapMode", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 4);
    EXPECT_EQ(call("GdipGetTextureWrapMode", {I(b), I(0)}), Status::InvalidParameter);
}

TEST_P(Gdip_pen_brush_Test, TextureSourceRect) {
    const u64 img = probe_texture_image();
    struct Case { float x, y, w, h; Status st; i32 iw, ih; u32 px00; };
    const Case cases[] = {
        {1, 0, 2, 2, Status::Ok, 2, 2, 0xff400011}, {0, 0, 4, 2, Status::Ok, 4, 2, 0xff000011},
        {3, 0, 2, 2, Status::OutOfMemory, 0, 0, 0}, {0, 0, 0, 2, Status::OutOfMemory, 0, 0, 0},
        {-1, 0, 2, 2, Status::OutOfMemory, 0, 0, 0}, {0.5f, 0.5f, 2, 1, Status::Ok, 2, 1, 0xff408011},
        {1, 1, 3, 1, Status::Ok, 3, 1, 0xff408011}, {0, 0, 5, 2, Status::OutOfMemory, 0, 0, 0},
        {0, 0, -2, 2, Status::OutOfMemory, 0, 0, 0},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(call("GdipCreateTexture2", {I(img), I(0), R(c.x), R(c.y), R(c.w), R(c.h), I(OUT)}), c.st)
            << c.x << "," << c.y << "," << c.w << "," << c.h;
        EXPECT_EQ(call("GdipCreateTexture2I", {I(img), I(0), I(static_cast<u64>(static_cast<i32>(c.x)) & 0xffffffff),
                                               I(static_cast<u64>(static_cast<i32>(c.y)) & 0xffffffff),
                                               I(static_cast<u64>(static_cast<i32>(c.w)) & 0xffffffff),
                                               I(static_cast<u64>(static_cast<i32>(c.h)) & 0xffffffff), I(OUT2)}),
                  c.st);
        if (c.st != Status::Ok) continue;
        ASSERT_EQ(call("GdipCreateTexture2", {I(img), I(0), R(c.x), R(c.y), R(c.w), R(c.h), I(OUT)}), Status::Ok);
        ASSERT_EQ(call("GdipGetTextureImage", {I(read_handle(OUT)), I(OUT2)}), Status::Ok);
        const GpImage* t = mgr.objects().get<GpImage>(read_handle(OUT2));
        EXPECT_EQ(t->width, c.iw);
        EXPECT_EQ(t->height, c.ih);
        EXPECT_EQ(px(read_handle(OUT2), 0, 0), c.px00);  // opaque: PARGB bytes = ARGB
    }
    EXPECT_EQ(call("GdipCreateTexture2", {I(img), I(5), R(0), R(0), R(1), R(1), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreateTexture2", {I(0), I(0), R(0), R(0), R(1), R(1), I(OUT)}), Status::InvalidParameter);
    // IA forms: NULL attributes = WrapModeTile; rect rules as above.
    const u64 ia = make_brush("GdipCreateTextureIA", {I(img), I(0), R(0), R(0), R(4), R(2), I(OUT)});
    EXPECT_EQ(call("GdipGetTextureWrapMode", {I(ia), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("GdipCreateTextureIA", {I(0), I(0), R(0), R(0), R(1), R(1), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateTextureIA", {I(img), I(0), R(2), R(0), R(4), R(2), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreateTextureIAI", {I(img), I(0), I(1), I(0), I(2), I(2), I(OUT)}), Status::Ok);
    // With attributes: their wrap mode (fresh: Clamp) and colour matrix.
    gdip::GpImageAttributes attrs;
    const u64 ah = mgr.objects().add(attrs);
    const u64 fresh = make_brush("GdipCreateTextureIA", {I(img), I(ah), R(0), R(0), R(4), R(2), I(OUT)});
    EXPECT_EQ(call("GdipGetTextureWrapMode", {I(fresh), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 4);  // "textureIA fresh-attr st=0 wrap 4"
    gdip::GpImageAttributes* a = mgr.objects().get<gdip::GpImageAttributes>(ah);
    a->wrap_mode = 3;
    gdip::ColorAdjust& d = a->adjust[0];  // Default category: swap R and B
    d.set = true;
    d.matrix_enabled = true;
    const float swap[5][5] = {{0, 0, 1, 0, 0}, {0, 1, 0, 0, 0}, {1, 0, 0, 0, 0}, {0, 0, 0, 1, 0}, {0, 0, 0, 0, 1}};
    std::memcpy(d.matrix.m, swap, sizeof swap);
    const u64 sw = make_brush("GdipCreateTextureIAI", {I(img), I(ah), I(0), I(0), I(4), I(2), I(OUT)});
    EXPECT_EQ(call("GdipGetTextureWrapMode", {I(sw), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    ASSERT_EQ(call("GdipGetTextureImage", {I(sw), I(OUT2)}), Status::Ok);
    EXPECT_EQ(px(read_handle(OUT2), 2, 1), 0xff118080u);  // "textureIAI matrix ... px21=ff118080"
    expect_row(fill(sw, 8, 4, {}), "textureIAI matrix fill y=1:", 1, false, 1);
}

TEST_P(Gdip_pen_brush_Test, TextureTransformOps) {
    const u64 b = make_brush("GdipCreateTexture", {I(probe_texture_image()), I(0), I(OUT)});
    const u64 m = matrix({0, 0, 0, 0, 0, 0});
    EXPECT_EQ(call("GdipGetTextureTransform", {I(b), I(m)}), Status::Ok);
    expect_matrix(matrix_of(m), {1, 0, 0, 1, 0, 0});
    const u64 sing = matrix({1, 2, 2, 4, 0, 0}), t56 = matrix({1, 0, 0, 1, 5, 6}), s23 = matrix({2, 0, 0, 3, 0, 0});
    EXPECT_EQ(call("GdipSetTextureTransform", {I(b), I(sing)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetTextureTransform", {I(b), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetTextureTransform", {I(b), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipMultiplyTextureTransform", {I(b), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipMultiplyTextureTransform", {I(b), I(sing), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipTranslateTextureTransform", {I(b), R(1), R(1), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipScaleTextureTransform", {I(b), R(1), R(1), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipRotateTextureTransform", {I(b), R(1), I(2)}), Status::InvalidParameter);
    // Multiply: order 2 appends ("texture s(2,3) *t(5,6) order2 m=[2 0 0 3 5 6]").
    for (auto [order, e] : {std::pair<u64, std::vector<float>>{2, {2, 0, 0, 3, 5, 6}},
                            {0, {2, 0, 0, 3, 10, 18}}, {1, {2, 0, 0, 3, 5, 6}}}) {
        ASSERT_EQ(call("GdipSetTextureTransform", {I(b), I(s23)}), Status::Ok);
        EXPECT_EQ(call("GdipMultiplyTextureTransform", {I(b), I(t56), I(order)}), Status::Ok);
        EXPECT_EQ(call("GdipGetTextureTransform", {I(b), I(m)}), Status::Ok);
        expect_matrix(matrix_of(m), e);
    }
    ASSERT_EQ(call("GdipResetTextureTransform", {I(b)}), Status::Ok);
    EXPECT_EQ(call("GdipTranslateTextureTransform", {I(b), R(1), R(2), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipScaleTextureTransform", {I(b), R(2), R(3), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipGetTextureTransform", {I(b), I(m)}), Status::Ok);
    expect_matrix(matrix_of(m), {2, 0, 0, 3, 1, 2});
    EXPECT_EQ(call("GdipRotateTextureTransform", {I(b), R(90), I(1)}), Status::Ok);
    EXPECT_EQ(call("GdipGetTextureTransform", {I(b), I(m)}), Status::Ok);
    expect_matrix(matrix_of(m), {0, 2, -3, 0, -2, 1});
    EXPECT_EQ(call("GdipMultiplyTextureTransform", {I(b), I(t56), I(1)}), Status::Ok);
    EXPECT_EQ(call("GdipMultiplyTextureTransform", {I(b), I(t56), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipRotateTextureTransform", {I(b), R(30), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipGetTextureTransform", {I(b), I(m)}), Status::Ok);
    expect_matrix(matrix_of(m), {-1.5f, 1.73205f, -2.59808f, -1, -15, 17});
    EXPECT_EQ(call("GdipScaleTextureTransform", {I(b), R(0), R(1), I(0)}), Status::Ok);  // allowed
    EXPECT_EQ(call("GdipSetTextureTransform", {I(b), I(t56)}), Status::Ok);
    EXPECT_EQ(call("GdipGetTextureTransform", {I(b), I(m)}), Status::Ok);
    expect_matrix(matrix_of(m), {1, 0, 0, 1, 5, 6});
}

TEST_P(Gdip_pen_brush_Test, TextureFillMatchesWindows) {
    const u64 img = probe_texture_image();
    for (i32 w = 0; w <= 4; ++w) {
        const u64 b = make_brush("GdipCreateTexture", {I(img), I(static_cast<u64>(w)), I(OUT)});
        const u64 out = fill(b, 12, 6, {0, 0, 12, 6});
        for (i32 y = 0; y < 5; ++y)
            expect_row(out, "texfill wrap" + std::to_string(w) + " y=" + std::to_string(y) + ":", y, false, 0);
    }
    const u64 b = make_brush("GdipCreateTexture", {I(img), I(0), I(OUT)});
    brush(b)->transform = {2, 0, 0, 1, 0, 0};
    expect_row(fill(b, 12, 6, {0, 0, 12, 6}), "texfill scale2x y=0:", 0, false, 1);
    brush(b)->transform = {1, 0, 0, 1, 1, 1};
    const u64 tr = fill(b, 12, 6, {0, 0, 12, 6});
    expect_row(tr, "texfill translate1,1 y=0:", 0, false, 0);
    expect_row(tr, "texfill translate1,1 y=1:", 1, false, 0);
    ASSERT_EQ(call("GdipResetTextureTransform", {I(b)}), Status::Ok);
    ASSERT_EQ(call("GdipRotateTextureTransform", {I(b), R(90), I(0)}), Status::Ok);
    const u64 rot = fill(b, 12, 6, {0, 0, 12, 6});
    for (i32 y = 0; y < 5; ++y) expect_row(rot, "texfill rot90 y=" + std::to_string(y) + ":", y, false, 0);
    ASSERT_EQ(call("GdipResetTextureTransform", {I(b)}), Status::Ok);
    // The texture follows the world transform, not the rendering origin.
    expect_row(fill(b, 12, 6, {-1, 0, 12, 6}, [](gdip::GraphicsState& s) { s.world.dx = 1; }),
               "texfill world+1 y=0:", 0, false, 0);
    expect_row(fill(b, 12, 6, {0, 0, 12, 6}, [](gdip::GraphicsState& s) { s.rendering_origin_x = 1; }),
               "texfill origin1 y=0:", 0, false, 0);
    // Half pixel offset samples the same texels ("texfill pixoffhalf").
    const u64 img2 = bitmap(4, 2);
    for (i32 y = 0; y < 2; ++y)
        for (i32 x = 0; x < 4; ++x) set_px(img2, x, y, 0xFF000000u | static_cast<u32>(x * 0x40) << 16 | static_cast<u32>(y * 0x80) << 8);
    const u64 b2 = make_brush("GdipCreateTexture", {I(img2), I(0), I(OUT)});
    expect_row(fill(b2, 12, 4, {0, 0, 12, 4}, [](gdip::GraphicsState& s) { s.pixel_offset_mode = 4; }),
               "texfill pixoffhalf y=0:", 0, false, 0);
    // Bilinear always (even with NearestNeighbor), mixing with transparent when clamped.
    brush(b2)->transform = {2, 0, 0, 1, 0, 0};
    expect_row(fill(b2, 12, 4, {0, 0, 12, 4}, [](gdip::GraphicsState& s) { s.interpolation_mode = 5; }),
               "texfill nearest scale2 y=0:", 0, false, 1);
    brush(b2)->wrap = gdip::WrapModeClamp;
    expect_row(fill(b2, 12, 4, {0, 0, 12, 4}), "texfill clamp scale2 y=0:", 0, false, 2);
    // Source rect, and the brush keeps its own pixels.
    const u64 src = make_brush("GdipCreateTexture2", {I(img), I(0), R(1), R(0), R(2), R(2), I(OUT)});
    expect_row(fill(src, 12, 6, {0, 0, 12, 6}), "texfill src1,0,2,2 y=0:", 0, false, 0);
    const u64 tmp = bitmap(2, 2);
    set_px(tmp, 0, 0, 0xff00ff00);
    const u64 own = make_brush("GdipCreateTexture", {I(tmp), I(0), I(OUT)});
    set_px(tmp, 0, 0, 0xffff0000);
    mgr.objects().remove<GpImage>(tmp);
    expect_row(fill(own, 12, 6, {0, 0, 2, 2}), "texfill image changed+disposed y=0:", 0, false, 0);
    // A clone survives the original.
    ASSERT_EQ(call("GdipCloneBrush", {I(src), I(OUT2)}), Status::Ok);
    const u64 clone = read_handle(OUT2);
    ASSERT_EQ(call("GdipDeleteBrush", {I(src)}), Status::Ok);
    EXPECT_EQ(call("GdipGetTextureImage", {I(clone), I(OUT3)}), Status::Ok);
    EXPECT_EQ(mgr.objects().get<GpImage>(read_handle(OUT3))->width, 2);
}

// ---- LinearGradientBrush ----------------------------------------------------------

TEST_P(Gdip_pen_brush_Test, LineCreateRules) {
    point_buf(BUF, {0, 0});
    point_buf(BUF + 8, {10, 0});
    EXPECT_EQ(call("GdipCreateLineBrush", {I(0), I(BUF + 8), I(0), I(0), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateLineBrush", {I(BUF), I(0), I(0), I(0), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0), I(0), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0), I(0), I(4), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateLineBrush", {I(BUF), I(BUF), I(0), I(0), I(0), I(OUT)}), Status::OutOfMemory);
    // Other out-of-range wrap modes are kept ("line wrap -1 st=0 get -1").
    for (u32 w : {0xffffffffu, 0u, 1u, 2u, 3u, 5u}) {
        const u64 b = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0), I(0), I(w), I(OUT)});
        EXPECT_EQ(call("GdipGetLineWrapMode", {I(b), I(OUT2)}), Status::Ok);
        EXPECT_EQ(static_cast<u32>(read_int(OUT2)), w);
    }
    const u64 b = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0xffff0000), I(0xff0000ff), I(0), I(OUT)});
    EXPECT_EQ(call("GdipGetBrushType", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 4);
    EXPECT_EQ(call("GdipGetLineColors", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0xffff0000u);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2 + 4)), 0xff0000ffu);
    EXPECT_EQ(call("GdipGetLineGammaCorrection", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    for (const char* fn : {"GdipGetLineColors", "GdipGetLineRect", "GdipGetLineRectI", "GdipGetLineGammaCorrection",
                           "GdipGetLineBlendCount", "GdipGetLinePresetBlendCount", "GdipGetLineWrapMode"}) {
        EXPECT_EQ(call(fn, {I(b), I(0)}), Status::InvalidParameter) << fn;
    }
    EXPECT_EQ(call("GdipSetLineWrapMode", {I(b), I(4)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetLineWrapMode", {I(b), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipSetLineWrapMode", {I(b), I(5)}), Status::Ok);
    EXPECT_EQ(call("GdipGetLineWrapMode", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(call("GdipSetLineGammaCorrection", {I(b), I(5)}), Status::Ok);
    EXPECT_EQ(call("GdipGetLineGammaCorrection", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 5);
    EXPECT_EQ(call("GdipSetLineColors", {I(b), I(0x11223344), I(0x55667788)}), Status::Ok);
    EXPECT_EQ(call("GdipGetLineColors", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0x11223344u);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2 + 4)), 0x55667788u);
}

TEST_P(Gdip_pen_brush_Test, LineGeometryMatchesWindows) {
    struct Case { float x1, y1, x2, y2; RectF r; std::vector<float> m; i32 ri[4]; };
    const Case cases[] = {
        {0, 0, 10, 0, {0, -5, 10, 10}, {1, 0, 0, 1, 0, 0}, {0, -5, 10, 10}},
        {1, 2, 5, 8, {1, 2, 4, 6}, {1, 1.5f, -0.923077f, 0.615385f, 4.61538f, -2.57692f}, {1, 2, 4, 6}},
        {10, 0, 0, 0, {0, -5, 10, 10}, {-1, 0, 0, -1, 10, 0}, {0, -5, 10, 10}},
        {0, 0, 0, 10, {-5, 0, 10, 10}, {0, 1, -1, 0, 5, 5}, {-5, 0, 10, 10}},
        {3, 3, -2, 7, {-2, 3, 5, 4}, {-1, 0.8f, -0.97561f, -1.21951f, 5.87805f, 10.6976f}, {-2, 3, 5, 4}},
        {0.5f, 0.25f, 4.75f, 0.25f, {0.5f, -1.875f, 4.25f, 4.25f}, {1, 0, 0, 1, 0, 0}, {1, -2, 4, 4}},
    };
    const u64 mx = matrix({});
    for (const Case& c : cases) {
        point_buf(BUF, {c.x1, c.y1});
        point_buf(BUF + 8, {c.x2, c.y2});
        const u64 b = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0xffff0000), I(0xff0000ff), I(0), I(OUT)});
        ASSERT_EQ(call("GdipGetLineRect", {I(b), I(OUT2)}), Status::Ok);
        EXPECT_FLOAT_EQ(read_real(OUT2), c.r.X);
        EXPECT_FLOAT_EQ(read_real(OUT2 + 4), c.r.Y);
        EXPECT_FLOAT_EQ(read_real(OUT2 + 8), c.r.Width);
        EXPECT_FLOAT_EQ(read_real(OUT2 + 12), c.r.Height);
        ASSERT_EQ(call("GdipGetLineRectI", {I(b), I(OUT2)}), Status::Ok);
        for (int i = 0; i < 4; ++i) EXPECT_EQ(read_int(OUT2 + 4 * static_cast<u64>(i)), c.ri[i]);
        ASSERT_EQ(call("GdipGetLineTransform", {I(b), I(mx)}), Status::Ok);
        expect_matrix(matrix_of(mx), {c.m[0], c.m[1], c.m[2], c.m[3], c.m[4], c.m[5]}, 2e-4f);
    }
    write_ints(BUF, {1, 2});
    write_ints(BUF + 8, {5, 8});
    const u64 bi = make_brush("GdipCreateLineBrushI", {I(BUF), I(BUF + 8), I(0), I(0), I(0), I(OUT)});
    ASSERT_EQ(call("GdipGetLineTransform", {I(bi), I(mx)}), Status::Ok);
    expect_matrix(matrix_of(mx), {1, 1.5f, -0.923077f, 0.615385f, 4.61538f, -2.57692f}, 2e-4f);
    EXPECT_EQ(call("GdipCreateLineBrushI", {I(BUF), I(BUF), I(0), I(0), I(0), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreateLineBrushI", {I(0), I(BUF), I(0), I(0), I(0), I(OUT)}), Status::InvalidParameter);

    // FromRect modes on (1,2,10,4): "rect mode N xform" rows.
    write_reals(BUF, {1, 2, 10, 4});
    const std::vector<float> modes[4] = {{1, 0, 0, 1, 0, 0}, {0, 0.4f, -2.5f, 0, 16, 1.6f},
                                         {1, 0.4f, -2.5f, 1, 10, -2.4f}, {-1, 0.4f, -2.5f, -1, 22, 5.6f}};
    for (u32 mode = 0; mode < 4; ++mode) {
        const u64 b = make_brush("GdipCreateLineBrushFromRect", {I(BUF), I(0), I(0), I(mode), I(0), I(OUT)});
        ASSERT_EQ(call("GdipGetLineTransform", {I(b), I(mx)}), Status::Ok);
        const auto& e = modes[mode];
        expect_matrix(matrix_of(mx), {e[0], e[1], e[2], e[3], e[4], e[5]});
        ASSERT_EQ(call("GdipGetLineRect", {I(b), I(OUT2)}), Status::Ok);
        EXPECT_FLOAT_EQ(read_real(OUT2 + 8), 10);
    }
    EXPECT_EQ(call("GdipCreateLineBrushFromRect", {I(BUF), I(0), I(0), I(0xffffffff), I(0), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreateLineBrushFromRect", {I(BUF), I(0), I(0), I(4), I(0), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreateLineBrushFromRect", {I(0), I(0), I(0), I(0), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateLineBrushFromRect", {I(BUF), I(0), I(0), I(0), I(4), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateLineBrushFromRect", {I(BUF), I(0), I(0), I(0), I(5), I(OUT)}), Status::Ok);
    write_reals(BUF + 16, {1, 2, 0, 4});
    EXPECT_EQ(call("GdipCreateLineBrushFromRect", {I(BUF + 16), I(0), I(0), I(0), I(0), I(OUT)}), Status::OutOfMemory);
    write_reals(BUF + 16, {1, 2, 10, 0});
    EXPECT_EQ(call("GdipCreateLineBrushFromRect", {I(BUF + 16), I(0), I(0), I(0), I(0), I(OUT)}), Status::OutOfMemory);
    write_reals(BUF + 16, {1, 2, -10, 4});
    const u64 neg = make_brush("GdipCreateLineBrushFromRect", {I(BUF + 16), I(0), I(0), I(0), I(0), I(OUT)});
    ASSERT_EQ(call("GdipGetLineTransform", {I(neg), I(mx)}), Status::Ok);
    expect_matrix(matrix_of(mx), {1, 0, 0, 1, 0, 0});

    // WithAngle: "angle A scal S xform" rows.
    struct Angle { float a; u32 s; std::vector<float> m; };
    const Angle angles[] = {
        {30, 0, {0.923205f, 0.533013f, -1.05801f, 1.83253f, 4.69282f, -6.5282f}},
        {30, 1, {1.18301f, 0.273205f, -1.70753f, 1.18301f, 5.73205f, -2.37128f}},
        {45, 0, {0.7f, 0.7f, -1.75f, 1.75f, 8.8f, -7.2f}},
        {135, 0, {-0.7f, 0.7f, -1.75f, -1.75f, 17.2f, 6.8f}},
        {225, 1, {-1, -0.4f, 2.5f, -1, 2, 10.4f}},
        {300, 0, {0.423205f, -0.733013f, 2.30801f, 1.33253f, -5.77128f, 3.06795f}},
        {-30, 1, {1.18301f, -0.273205f, 1.70753f, 1.18301f, -7.9282f, 0.907179f}},
        {400, 0, {0.783786f, 0.657674f, -1.52534f, 1.81783f, 7.39866f, -7.21738f}},
        {89.9f, 1, {0.00174828f, 0.400697f, -2.50436f, 0.00174823f, 16.0069f, 1.58882f}},
    };
    for (const Angle& a : angles) {
        const u64 b = make_brush("GdipCreateLineBrushFromRectWithAngle",
                                 {I(BUF), I(0), I(0), R(a.a), I(a.s), I(0), I(OUT)});
        ASSERT_EQ(call("GdipGetLineTransform", {I(b), I(mx)}), Status::Ok);
        expect_matrix(matrix_of(mx), {a.m[0], a.m[1], a.m[2], a.m[3], a.m[4], a.m[5]}, 3e-4f);
    }
    write_reals(BUF + 16, {1, 2, 0, 4});
    EXPECT_EQ(call("GdipCreateLineBrushFromRectWithAngle", {I(BUF + 16), I(0), I(0), R(30), I(0), I(0), I(OUT)}),
              Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreateLineBrushFromRectWithAngle", {I(0), I(0), I(0), R(30), I(0), I(0), I(OUT)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateLineBrushFromRectWithAngle", {I(BUF), I(0), I(0), R(30), I(0), I(4), I(OUT)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateLineBrushFromRectWithAngle", {I(BUF), I(0), I(0), R(30), I(2), I(0), I(OUT)}), Status::Ok);
    write_ints(BUF + 16, {1, 2, 10, 4});
    const u64 ai = make_brush("GdipCreateLineBrushFromRectWithAngleI", {I(BUF + 16), I(0), I(0), R(30), I(0), I(0), I(OUT)});
    ASSERT_EQ(call("GdipGetLineTransform", {I(ai), I(mx)}), Status::Ok);
    expect_matrix(matrix_of(mx), {0.923205f, 0.533013f, -1.05801f, 1.83253f, 4.69282f, -6.5282f}, 2e-4f);
    const u64 ri = make_brush("GdipCreateLineBrushFromRectI", {I(BUF + 16), I(0), I(0), I(2), I(0), I(OUT)});
    ASSERT_EQ(call("GdipGetLineTransform", {I(ri), I(mx)}), Status::Ok);
    expect_matrix(matrix_of(mx), {1, 0.4f, -2.5f, 1, 10, -2.4f});
    // Reset drops the derived mapping; the rect stays ("line after reset").
    ASSERT_EQ(call("GdipResetLineTransform", {I(ri)}), Status::Ok);
    ASSERT_EQ(call("GdipGetLineTransform", {I(ri), I(mx)}), Status::Ok);
    expect_matrix(matrix_of(mx), {1, 0, 0, 1, 0, 0});
}

TEST_P(Gdip_pen_brush_Test, LineBlendRules) {
    point_buf(BUF, {0, 0});
    point_buf(BUF + 8, {10, 0});
    const u64 b = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0xffff0000), I(0xff0000ff), I(0), I(OUT)});
    const GuestAddr F = BUF + 0x100, P = BUF + 0x200, C = BUF + 0x300, OF = BUF + 0x400, OP = BUF + 0x500;
    // Fresh: one factor 1, no interpolation colours.
    EXPECT_EQ(call("GdipGetLineBlendCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
    EXPECT_EQ(call("GdipGetLineBlend", {I(b), I(OF), I(OP), I(0)}), Status::InvalidParameter);
    write_reals(OF, {9, 9});
    EXPECT_EQ(call("GdipGetLineBlend", {I(b), I(OF), I(OP), I(2)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OF), 1);
    EXPECT_FLOAT_EQ(read_real(OF + 4), 9);  // only the stored entry is written
    EXPECT_EQ(call("GdipGetLineBlend", {I(b), I(0), I(OP), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetLineBlend", {I(b), I(OF), I(0), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetLinePresetBlendCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("GdipGetLinePresetBlend", {I(b), I(C), I(P), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetLinePresetBlend", {I(b), I(C), I(P), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetLinePresetBlend", {I(b), I(C), I(P), I(2)}), Status::GenericError);
    // SetLineBlend.
    write_reals(F, {0, 1, 0.5f, 1});
    write_reals(P, {0, 0.3f, 1});
    EXPECT_EQ(call("GdipSetLineBlend", {I(b), I(F), I(P), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetLineBlend", {I(b), I(F), I(P), I(0xffffffff)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetLineBlend", {I(b), I(0), I(P), I(3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetLineBlend", {I(b), I(F), I(0), I(3)}), Status::InvalidParameter);
    write_reals(P + 0x40, {0.2f, 1});
    EXPECT_EQ(call("GdipSetLineBlend", {I(b), I(F), I(P + 0x40), I(2)}), Status::InvalidParameter);
    write_reals(P + 0x40, {0, 0.8f});
    EXPECT_EQ(call("GdipSetLineBlend", {I(b), I(F), I(P + 0x40), I(2)}), Status::InvalidParameter);
    write_reals(P + 0x40, {0, 0.8f, 0.3f, 1});  // non-monotonic middle is accepted
    EXPECT_EQ(call("GdipSetLineBlend", {I(b), I(F), I(P + 0x40), I(4)}), Status::Ok);
    write_reals(F + 0x40, {0, 2, -1});  // factors are not range-checked
    EXPECT_EQ(call("GdipSetLineBlend", {I(b), I(F + 0x40), I(P), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipSetLineBlend", {I(b), I(F), I(P), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipGetLineBlendCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(call("GdipGetLineBlend", {I(b), I(OF), I(OP), I(2)}), Status::InsufficientBuffer);
    EXPECT_EQ(call("GdipGetLineBlend", {I(b), I(OF), I(OP), I(5)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OF + 4), 1);
    EXPECT_FLOAT_EQ(read_real(OP + 4), 0.3f);
    // Interpolation colours.
    write_ints(C, {static_cast<i32>(0xffff0000), static_cast<i32>(0xff00ff00), static_cast<i32>(0xff0000ff)});
    EXPECT_EQ(call("GdipSetLinePresetBlend", {I(b), I(C), I(P), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetLinePresetBlend", {I(b), I(C), I(P), I(0)}), Status::InvalidParameter);
    write_reals(P + 0x40, {0.2f, 1});
    EXPECT_EQ(call("GdipSetLinePresetBlend", {I(b), I(C), I(P + 0x40), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetLinePresetBlend", {I(b), I(0), I(P), I(3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetLinePresetBlend", {I(b), I(C), I(P), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipGetLinePresetBlendCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(call("GdipGetLineBlendCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(call("GdipGetLinePresetBlend", {I(b), I(OF), I(OP), I(2)}), Status::InsufficientBuffer);
    EXPECT_EQ(call("GdipGetLinePresetBlend", {I(b), I(0), I(OP), I(3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetLinePresetBlend", {I(b), I(OF), I(0), I(3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetLinePresetBlend", {I(b), I(OF), I(OP), I(5)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OF + 4)), 0xff00ff00u);
    EXPECT_FLOAT_EQ(read_real(OP + 4), 0.3f);
    // SetLineColors keeps them; a clone copies them; a blend replaces them.
    EXPECT_EQ(call("GdipSetLineColors", {I(b), I(0xff111111), I(0xff222222)}), Status::Ok);
    EXPECT_EQ(call("GdipGetLinePresetBlendCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    ASSERT_EQ(call("GdipCloneBrush", {I(b), I(OUT3)}), Status::Ok);
    EXPECT_EQ(call("GdipSetLineBlend", {I(b), I(F), I(P), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipGetLinePresetBlendCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("GdipGetLinePresetBlendCount", {I(read_handle(OUT3)), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
}

TEST_P(Gdip_pen_brush_Test, SigmaAndLinearBlendTablesMatchWindows) {
    point_buf(BUF, {0, 0});
    point_buf(BUF + 8, {10, 0});
    const GuestAddr OF = BUF + 0x1000, OP = BUF + 0x2000;
    auto parse = [](const std::string& s) {
        std::vector<std::pair<float, float>> out;
        usize i = 0;
        while ((i = s.find('(', i)) != std::string::npos) {
            float f = 0, p = 0;
            std::sscanf(s.c_str() + i, "(%f,%f)", &f, &p);
            out.emplace_back(f, p);
            ++i;
        }
        return out;
    };
    struct Case { const char* tag; float focus, scale; };
    for (const Case& c : {Case{"0.5 1", 0.5f, 1}, Case{"0 1", 0, 1}, Case{"1 1", 1, 1}, Case{"0.3 0.6", 0.3f, 0.6f},
                          Case{"0.7 0", 0.7f, 0}}) {
        for (const char* kind : {"sigma", "linear"}) {
            for (const char* area : {"line", "pg"}) {
                const bool line = std::string(area) == "line";
                if (!line && std::string(c.tag) == "0.7 0") continue;  // not captured for pg
                u64 b;
                if (line) {
                    b = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0), I(0), I(0), I(OUT)});
                } else {
                    write_reals(BUF + 0x40, {0, 0, 20, 0, 20, 20, 0, 20});
                    b = make_brush("GdipCreatePathGradient", {I(BUF + 0x40), I(4), I(0), I(OUT)});
                }
                const std::string fn = std::string(line ? "GdipSetLine" : "GdipSetPathGradient") +
                                       (std::string(kind) == "sigma" ? "SigmaBlend" : "LinearBlend");
                ASSERT_EQ(call(fn, {I(b), R(c.focus), R(c.scale)}), Status::Ok) << fn;
                const auto want = parse(probe_line(std::string(area) + " " + kind + " " + c.tag + " blend st=0:"));
                ASSERT_EQ(call(line ? "GdipGetLineBlendCount" : "GdipGetPathGradientBlendCount", {I(b), I(OUT2)}),
                          Status::Ok);
                ASSERT_EQ(static_cast<usize>(read_int(OUT2)), want.size()) << area << " " << kind << " " << c.tag;
                ASSERT_EQ(call(line ? "GdipGetLineBlend" : "GdipGetPathGradientBlend",
                               {I(b), I(OF), I(OP), I(want.size())}), Status::Ok);
                for (usize i = 0; i < want.size(); ++i) {
                    EXPECT_NEAR(read_real(OF + i * 4), want[i].first, 3e-5f) << kind << " " << c.tag << " " << i;
                    EXPECT_NEAR(read_real(OP + i * 4), want[i].second, 2e-6f) << kind << " " << c.tag << " " << i;
                }
            }
        }
    }
    const u64 b = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0), I(0), I(0), I(OUT)});
    for (auto [f, s] : {std::pair{-0.1f, 1.0f}, {1.1f, 1.0f}, {0.5f, -0.1f}, {0.5f, 1.1f}}) {
        EXPECT_EQ(call("GdipSetLineSigmaBlend", {I(b), R(f), R(s)}), Status::InvalidParameter);
        EXPECT_EQ(call("GdipSetLineLinearBlend", {I(b), R(f), R(s)}), Status::InvalidParameter);
    }
}

TEST_P(Gdip_pen_brush_Test, LineFillMatchesWindows) {
    point_buf(BUF, {0, 0});
    point_buf(BUF + 8, {10, 0});
    for (u32 w = 0; w <= 3; ++w) {
        const u64 b = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0xffff0000), I(0xff0000ff), I(w), I(OUT)});
        const u64 img = fill(b, 40, 12, {0, 0, 40, 12});
        expect_row(img, "linefill wrap" + std::to_string(w) + " y=0:", 0, false, 1);
        expect_row(img, "linefill wrap" + std::to_string(w) + " y=11:", 11, false, 1);
    }
    const u64 b = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0xffff0000), I(0xff0000ff), I(0), I(OUT)});
    ASSERT_EQ(call("GdipSetLineGammaCorrection", {I(b), I(1)}), Status::Ok);
    expect_row(fill(b, 40, 12, {0, 0, 40, 12}), "linefill gamma y=0:", 0, false, 1);
    ASSERT_EQ(call("GdipSetLineGammaCorrection", {I(b), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipSetLineSigmaBlend", {I(b), R(0.5f), R(1)}), Status::Ok);
    expect_row(fill(b, 40, 12, {0, 0, 40, 12}), "linefill sigma.5 y=0:", 0, false, 1);
    ASSERT_EQ(call("GdipSetLineLinearBlend", {I(b), R(0.3f), R(0.8f)}), Status::Ok);
    expect_row(fill(b, 40, 12, {0, 0, 40, 12}), "linefill linear.3,.8 y=0:", 0, false, 1);
    write_ints(BUF + 0x40, {static_cast<i32>(0xffff0000), static_cast<i32>(0xff00ff00), static_cast<i32>(0xff0000ff)});
    write_reals(BUF + 0x80, {0, 0.3f, 1});
    ASSERT_EQ(call("GdipSetLinePresetBlend", {I(b), I(BUF + 0x40), I(BUF + 0x80), I(3)}), Status::Ok);
    expect_row(fill(b, 40, 12, {0, 0, 40, 12}), "linefill preset y=0:", 0, false, 1);
    ASSERT_EQ(call("GdipSetLineGammaCorrection", {I(b), I(1)}), Status::Ok);
    expect_row(fill(b, 40, 12, {0, 0, 40, 12}), "linefill preset gamma y=0:", 0, false, 1);
    // One blend factor acts as an exponent ("linefill blend1 .5").
    const u64 one = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0xffff0000), I(0xff0000ff), I(0), I(OUT)});
    write_reals(BUF + 0xC0, {0.5f, 0});
    ASSERT_EQ(call("GdipSetLineBlend", {I(one), I(BUF + 0xC0), I(BUF + 0xC4), I(1)}), Status::Ok);
    expect_row(fill(one, 40, 4, {0, 0, 40, 4}), "linefill blend1 .5 y=0:", 0, false, 1);
    // The 17-entry table smooths knots the same at any length ("linefill long preset").
    point_buf(BUF + 16, {1000, 0});
    const u64 lng = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 16), I(0xffff0000), I(0xff0000ff), I(0), I(OUT)});
    write_ints(BUF + 0x40, {static_cast<i32>(0xffff0000), static_cast<i32>(0xff00ff00), static_cast<i32>(0xff0000ff)});
    ASSERT_EQ(call("GdipSetLinePresetBlend", {I(lng), I(BUF + 0x40), I(BUF + 0x80), I(3)}), Status::Ok);
    const u64 limg = fill(lng, 1000, 1, {0, 0, 1000, 1});
    const std::vector<u32> want = probe_row("linefill long preset y=0:");
    for (i32 i = 0; i < 9; ++i) {
        const u32 got = px(limg, 296 + i, 0), exp = want[static_cast<usize>(i)];
        for (int c = 0; c < 32; c += 8)
            EXPECT_LE(std::abs(static_cast<i32>((got >> c) & 0xFF) - static_cast<i32>((exp >> c) & 0xFF)), 1)
                << std::hex << got << " " << exp;
    }
    // Premultiplied interpolation of alpha colours.
    const u64 a = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0x80ff0000), I(0xff0000ff), I(0), I(OUT)});
    expect_row(fill(a, 40, 12, {0, 0, 40, 12}), "linefill alpha y=0:", 0, false, 2);
    // Brush transform, world transform, pixel offset.
    const u64 plain = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0xffff0000), I(0xff0000ff), I(0), I(OUT)});
    ASSERT_EQ(call("GdipTranslateLineTransform", {I(plain), R(3), R(0), I(0)}), Status::Ok);
    expect_row(fill(plain, 40, 12, {0, 0, 40, 12}), "linefill xlate3 y=0:", 0, false, 1);
    ASSERT_EQ(call("GdipResetLineTransform", {I(plain)}), Status::Ok);
    ASSERT_EQ(call("GdipScaleLineTransform", {I(plain), R(2), R(1), I(0)}), Status::Ok);
    expect_row(fill(plain, 40, 12, {0, 0, 40, 12}), "linefill scale2 y=0:", 0, false, 1);
    ASSERT_EQ(call("GdipResetLineTransform", {I(plain)}), Status::Ok);
    expect_row(fill(plain, 40, 12, {-2, 0, 40, 12}, [](gdip::GraphicsState& s) { s.world.dx = 2; }),
               "linefill world+2 y=0:", 0, false, 1);
    expect_row(fill(plain, 12, 4, {0, 0, 12, 4}, [](gdip::GraphicsState& s) { s.pixel_offset_mode = 4; }),
               "linefill pixoffhalf y=0:", 0, false, 1);
    write_reals(BUF + 0x100, {0, 0, 10, 4});
    const u64 v = make_brush("GdipCreateLineBrushFromRect", {I(BUF + 0x100), I(0xffff0000), I(0xff0000ff), I(1), I(0), I(OUT)});
    expect_row(fill(v, 40, 12, {0, 0, 40, 12}), "linefill vertical x=0:", 0, true, 1);
    const u64 fd = make_brush("GdipCreateLineBrushFromRect", {I(BUF + 0x100), I(0xffff0000), I(0xff0000ff), I(2), I(0), I(OUT)});
    const u64 fdi = fill(fd, 40, 12, {0, 0, 40, 12});
    for (i32 y = 0; y < 4; ++y) expect_row(fdi, "linefill fdiag y=" + std::to_string(y) + ":", y, false, 1);
    const u64 an = make_brush("GdipCreateLineBrushFromRectWithAngle",
                              {I(BUF + 0x100), I(0xffff0000), I(0xff0000ff), R(30), I(1), I(0), I(OUT)});
    const u64 ani = fill(an, 40, 12, {0, 0, 40, 12});
    for (i32 y = 0; y < 4; ++y) expect_row(ani, "linefill angle30s y=" + std::to_string(y) + ":", y, false, 1);
}

TEST_P(Gdip_pen_brush_Test, LineTransformOps) {
    point_buf(BUF, {0, 0});
    point_buf(BUF + 8, {10, 0});
    const u64 b = make_brush("GdipCreateLineBrush", {I(BUF), I(BUF + 8), I(0), I(0), I(0), I(OUT)});
    const u64 m = matrix({}), t56 = matrix({1, 0, 0, 1, 5, 6}), sing = matrix({1, 2, 2, 4, 0, 0});
    EXPECT_EQ(call("GdipSetLineTransform", {I(b), I(sing)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetLineTransform", {I(b), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetLineTransform", {I(b), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipMultiplyLineTransform", {I(b), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipTranslateLineTransform", {I(b), R(1), R(1), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipScaleLineTransform", {I(b), R(1), R(1), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipRotateLineTransform", {I(b), R(1), I(2)}), Status::InvalidParameter);
    // "line t(1,2)p", "s(2,3)a", "r90p", "*t(5,6)a" from [1 0 0 1 5 6].
    ASSERT_EQ(call("GdipSetLineTransform", {I(b), I(t56)}), Status::Ok);
    ASSERT_EQ(call("GdipTranslateLineTransform", {I(b), R(1), R(2), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipGetLineTransform", {I(b), I(m)}), Status::Ok);
    expect_matrix(matrix_of(m), {1, 0, 0, 1, 6, 8});
    ASSERT_EQ(call("GdipScaleLineTransform", {I(b), R(2), R(3), I(1)}), Status::Ok);
    ASSERT_EQ(call("GdipGetLineTransform", {I(b), I(m)}), Status::Ok);
    expect_matrix(matrix_of(m), {2, 0, 0, 3, 12, 24});
    ASSERT_EQ(call("GdipRotateLineTransform", {I(b), R(90), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipGetLineTransform", {I(b), I(m)}), Status::Ok);
    expect_matrix(matrix_of(m), {0, 3, -2, 0, 12, 24});
    ASSERT_EQ(call("GdipMultiplyLineTransform", {I(b), I(t56), I(1)}), Status::Ok);
    ASSERT_EQ(call("GdipGetLineTransform", {I(b), I(m)}), Status::Ok);
    expect_matrix(matrix_of(m), {0, 3, -2, 0, 17, 30});
    // The rect is not changed by transforms ("line after set rect").
    ASSERT_EQ(call("GdipGetLineRect", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT2 + 4), -5);
}

// ---- PathGradientBrush ----------------------------------------------------------

TEST_P(Gdip_pen_brush_Test, PathGradientCreateRulesAndDefaults) {
    write_reals(BUF, {0, 0, 10, 0, 0, 10});
    for (u32 n : {0xffffffffu, 0u, 1u, 2u})
        EXPECT_EQ(call("GdipCreatePathGradient", {I(BUF), I(n), I(0), I(OUT)}), Status::OutOfMemory) << n;
    EXPECT_EQ(call("GdipCreatePathGradient", {I(BUF), I(3), I(0xffffffff), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreatePathGradient", {I(BUF), I(3), I(5), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreatePathGradient", {I(0), I(3), I(0), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreatePathGradient", {I(BUF), I(3), I(0), I(0)}), Status::InvalidParameter);
    write_reals(BUF + 0x40, {0, 0, 5, 0, 10, 0});
    EXPECT_EQ(call("GdipCreatePathGradient", {I(BUF + 0x40), I(3), I(0), I(OUT)}), Status::OutOfMemory);
    write_reals(BUF + 0x40, {0, 0, 5, 5, 10, 10});
    EXPECT_EQ(call("GdipCreatePathGradient", {I(BUF + 0x40), I(3), I(0), I(OUT)}), Status::Ok);  // "pg diag"
    const u64 b = make_brush("GdipCreatePathGradient", {I(BUF), I(3), I(4), I(OUT)});
    EXPECT_EQ(call("GdipGetBrushType", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(call("GdipGetPathGradientCenterColor", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0xff000000u);
    EXPECT_EQ(call("GdipGetPathGradientCenterPoint", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_NEAR(read_real(OUT2), 3.333333f, 1e-5f);
    EXPECT_EQ(call("GdipGetPathGradientCenterPointI", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(call("GdipGetPathGradientPointCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(call("GdipGetPathGradientSurroundColorCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    write_int(OUT3, 3);
    EXPECT_EQ(call("GdipGetPathGradientSurroundColorsWithCount", {I(b), I(BUF + 0x100), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 1);  // "pg tri surround st=0 k=1"
    for (int i = 0; i < 3; ++i) EXPECT_EQ(static_cast<u32>(read_int(BUF + 0x100 + 4 * static_cast<u64>(i))), 0xffffffffu);
    EXPECT_EQ(call("GdipGetPathGradientRect", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT2 + 8), 10);
    EXPECT_EQ(call("GdipGetPathGradientWrapMode", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 4);
    EXPECT_EQ(call("GdipGetPathGradientGammaCorrection", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("GdipGetPathGradientFocusScales", {I(b), I(OUT2), I(OUT3)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT2), 0);
    EXPECT_EQ(call("GdipGetPathGradientBlendCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
    EXPECT_EQ(call("GdipGetPathGradientPresetBlendCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    // Vertex average, not the area centroid ("pg poly center 6 3").
    write_reals(BUF + 0x40, {0, 0, 12, 0, 12, 3, 0, 9});
    const u64 poly = make_brush("GdipCreatePathGradient", {I(BUF + 0x40), I(4), I(0), I(OUT)});
    EXPECT_EQ(call("GdipGetPathGradientCenterPoint", {I(poly), I(OUT2)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT2), 6);
    EXPECT_FLOAT_EQ(read_real(OUT2 + 4), 3);
    EXPECT_EQ(call("GdipGetPathGradientRectI", {I(poly), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2 + 12), 9);
    // INT form: NULL points is InvalidParameter there.
    write_ints(BUF + 0x80, {0, 0, 10, 0, 0, 10});
    EXPECT_EQ(call("GdipCreatePathGradientI", {I(BUF + 0x80), I(3), I(0), I(OUT)}), Status::Ok);
    EXPECT_EQ(call("GdipCreatePathGradientI", {I(BUF + 0x80), I(1), I(0), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreatePathGradientI", {I(0), I(3), I(0), I(OUT)}), Status::InvalidParameter);
    // From a path: white centre, Clamp, every figure in one point list.
    GpPath path;
    for (auto [x, y] : {std::pair{0.f, 0.f}, {8.f, 0.f}, {8.f, 8.f}, {0.f, 8.f}, {12.f, 0.f}, {20.f, 0.f}, {20.f, 8.f}, {12.f, 8.f}}) {
        path.points.push_back({x, y});
        path.types.push_back(path.types.size() % 4 == 0 ? 0 : (path.types.size() % 4 == 3 ? 0x81 : 1));
    }
    const u64 ph = mgr.objects().add(path);
    const u64 fp = make_brush("GdipCreatePathGradientFromPath", {I(ph), I(OUT)});
    EXPECT_EQ(call("GdipGetPathGradientCenterColor", {I(fp), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0xffffffffu);
    EXPECT_EQ(call("GdipGetPathGradientCenterPoint", {I(fp), I(OUT2)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT2), 10);
    EXPECT_FLOAT_EQ(read_real(OUT2 + 4), 4);
    EXPECT_EQ(call("GdipGetPathGradientPointCount", {I(fp), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 8);
    EXPECT_EQ(call("GdipGetPathGradientWrapMode", {I(fp), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 4);
    const u64 empty = mgr.objects().add(GpPath{});
    EXPECT_EQ(call("GdipCreatePathGradientFromPath", {I(empty), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreatePathGradientFromPath", {I(0), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreatePathGradientFromPath", {I(ph), I(0)}), Status::InvalidParameter);
    // GDI+ implements neither path accessor.
    EXPECT_EQ(call("GdipGetPathGradientPath", {I(fp), I(ph)}), Status::NotImplemented);
    EXPECT_EQ(call("GdipSetPathGradientPath", {I(fp), I(ph)}), Status::NotImplemented);
    EXPECT_EQ(call("GdipGetPathGradientPath", {I(0), I(0)}), Status::NotImplemented);
}

TEST_P(Gdip_pen_brush_Test, PathGradientSetters) {
    write_reals(BUF, {0, 0, 20, 0, 20, 20, 0, 20});
    const u64 b = make_brush("GdipCreatePathGradient", {I(BUF), I(4), I(0), I(OUT)});
    const GuestAddr C = BUF + 0x100, OC = BUF + 0x200;
    write_ints(C, {static_cast<i32>(0xff0000ff), static_cast<i32>(0xff00ff00), static_cast<i32>(0xffff0000),
                   static_cast<i32>(0xff000000), static_cast<i32>(0xffffffff)});
    for (i32 bad : {0, 5}) {
        write_int(OUT2, bad);
        EXPECT_EQ(call("GdipSetPathGradientSurroundColorsWithCount", {I(b), I(C), I(OUT2)}), Status::InvalidParameter);
    }
    write_int(OUT2, 2);
    EXPECT_EQ(call("GdipSetPathGradientSurroundColorsWithCount", {I(b), I(C), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 2);
    for (i32 k = 1; k <= 5; ++k) {
        write_int(OUT3, k);
        for (int i = 0; i < 5; ++i) write_int(OC + 4 * static_cast<u64>(i), 0x11111111);
        const Status st = call("GdipGetPathGradientSurroundColorsWithCount", {I(b), I(OC), I(OUT3)});
        EXPECT_EQ(st, k < 4 ? Status::InvalidParameter : Status::Ok) << k;
        if (st != Status::Ok) continue;
        EXPECT_EQ(read_int(OUT3), 2);  // "pg stored2 get k4 st=0 k=2"
        EXPECT_EQ(static_cast<u32>(read_int(OC)), 0xff0000ffu);
        EXPECT_EQ(static_cast<u32>(read_int(OC + 12)), 0xff00ff00u);
        EXPECT_EQ(static_cast<u32>(read_int(OC + 16)), 0x11111111u);
    }
    EXPECT_EQ(call("GdipGetPathGradientSurroundColorCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 4);
    // Centre point: GpRound for the INT getter.
    struct Rnd { float x, y; i32 ix, iy; };
    for (const Rnd& r : {Rnd{2.5f, -2.5f, 3, -2}, Rnd{3.5f, 1.49f, 4, 1}, Rnd{100, 100, 100, 100}}) {
        write_reals(OUT2, {r.x, r.y});
        EXPECT_EQ(call("GdipSetPathGradientCenterPoint", {I(b), I(OUT2)}), Status::Ok);
        EXPECT_EQ(call("GdipGetPathGradientCenterPointI", {I(b), I(OUT3)}), Status::Ok);
        EXPECT_EQ(read_int(OUT3), r.ix);
        EXPECT_EQ(read_int(OUT3 + 4), r.iy);
    }
    write_ints(OUT2, {7, -2});
    EXPECT_EQ(call("GdipSetPathGradientCenterPointI", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPathGradientCenterPoint", {I(b), I(OUT3)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT3), 7);
    EXPECT_EQ(call("GdipSetPathGradientCenterPoint", {I(b), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPathGradientCenterPointI", {I(b), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPathGradientCenterColor", {I(b), I(0x80ff00ff)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPathGradientCenterColor", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0x80ff00ffu);
    EXPECT_EQ(call("GdipGetPathGradientCenterColor", {I(b), I(0)}), Status::InvalidParameter);
    // Focus scales: unchecked.
    EXPECT_EQ(call("GdipSetPathGradientFocusScales", {I(b), R(0.5f), R(3)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPathGradientFocusScales", {I(b), I(OUT2), I(OUT3)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT3), 3);
    EXPECT_EQ(call("GdipGetPathGradientFocusScales", {I(b), I(0), I(OUT3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetPathGradientFocusScales", {I(b), I(OUT2), I(0)}), Status::InvalidParameter);
    // Wrap: unknown values ignored; gamma kept as given.
    EXPECT_EQ(call("GdipSetPathGradientWrapMode", {I(b), I(4)}), Status::Ok);
    EXPECT_EQ(call("GdipSetPathGradientWrapMode", {I(b), I(5)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPathGradientWrapMode", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 4);
    EXPECT_EQ(call("GdipSetPathGradientGammaCorrection", {I(b), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPathGradientGammaCorrection", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    // Blend / preset rules.
    write_reals(BUF + 0x300, {0, 1, 0.5f});
    write_reals(BUF + 0x340, {0, 0.3f, 1});
    EXPECT_EQ(call("GdipSetPathGradientBlend", {I(b), I(BUF + 0x300), I(BUF + 0x340), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPathGradientBlend", {I(b), I(BUF + 0x300), I(BUF + 0x340), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPathGradientBlend", {I(b), I(OC), I(OC + 0x40), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetPathGradientBlend", {I(b), I(OC), I(OC + 0x40), I(2)}), Status::InsufficientBuffer);
    EXPECT_EQ(call("GdipGetPathGradientPresetBlend", {I(b), I(OC), I(OC + 0x40), I(2)}), Status::GenericError);
    write_ints(C, {static_cast<i32>(0xffff0000), static_cast<i32>(0xff00ff00), static_cast<i32>(0xff0000ff)});
    write_reals(BUF + 0x340, {0, 0.5f, 1});
    EXPECT_EQ(call("GdipSetPathGradientPresetBlend", {I(b), I(C), I(BUF + 0x340), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPathGradientPresetBlend", {I(b), I(C), I(BUF + 0x340), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPathGradientBlendCount", {I(b), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    // Buffers are filled from the end ("pg getpreset count0..4").
    for (i32 count = 0; count <= 4; ++count) {
        for (int i = 0; i < 6; ++i) {
            write_int(OC + 4 * static_cast<u64>(i), 0x11111111);
            write_real(OC + 0x40 + 4 * static_cast<u64>(i), 9);
        }
        const Status st = call("GdipGetPathGradientPresetBlend", {I(b), I(OC), I(OC + 0x40), I(static_cast<u64>(count))});
        EXPECT_EQ(st, count < 2 ? Status::InvalidParameter : Status::Ok);
        if (count == 2) {
            EXPECT_EQ(static_cast<u32>(read_int(OC)), 0xff00ff00u);
            EXPECT_FLOAT_EQ(read_real(OC + 0x40), 0.5f);
            EXPECT_EQ(static_cast<u32>(read_int(OC + 4)), 0xff0000ffu);
        } else if (count == 4) {
            EXPECT_EQ(static_cast<u32>(read_int(OC + 4)), 0xffff0000u);
            EXPECT_EQ(static_cast<u32>(read_int(OC + 12)), 0xff0000ffu);
            EXPECT_FLOAT_EQ(read_real(OC + 0x40 + 12), 1);
        }
    }
    // Transforms: same rules as the other brushes.
    const u64 m = matrix({}), sing = matrix({1, 2, 2, 4, 0, 0}), t56 = matrix({1, 0, 0, 1, 5, 6});
    EXPECT_EQ(call("GdipSetPathGradientTransform", {I(b), I(sing)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPathGradientTransform", {I(b), I(0)}), Status::InvalidParameter);
    // Order 2 appends: the probe's brush starts at [1 0 0 1 5 6], and
    // then "pg t,r90a,s,*t m=[0 2 -3 0 -26 16]".
    EXPECT_EQ(call("GdipMultiplyPathGradientTransform", {I(b), I(t56), I(2)}), Status::Ok);
    ASSERT_EQ(call("GdipTranslatePathGradientTransform", {I(b), R(1), R(2), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipRotatePathGradientTransform", {I(b), R(90), I(1)}), Status::Ok);
    ASSERT_EQ(call("GdipScalePathGradientTransform", {I(b), R(2), R(3), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipMultiplyPathGradientTransform", {I(b), I(t56), I(0)}), Status::Ok);
    ASSERT_EQ(call("GdipGetPathGradientTransform", {I(b), I(m)}), Status::Ok);
    expect_matrix(matrix_of(m), {0, 2, -3, 0, -26, 16});
}

TEST_P(Gdip_pen_brush_Test, PathGradientFillMatchesWindows) {
    write_reals(BUF, {0, 0, 20, 0, 20, 20, 0, 20});
    write_int(BUF + 0x80, static_cast<i32>(0xff0000ff));
    auto square = [&](u32 wrap) {
        const u64 b = make_brush("GdipCreatePathGradient", {I(BUF), I(4), I(wrap), I(OUT)});
        EXPECT_EQ(call("GdipSetPathGradientCenterColor", {I(b), I(0xffff0000)}), Status::Ok);
        write_int(OUT2, 1);
        EXPECT_EQ(call("GdipSetPathGradientSurroundColorsWithCount", {I(b), I(BUF + 0x80), I(OUT2)}), Status::Ok);
        return b;
    };
    // GDI+'s own interpolation inside the fan triangles carries up to ~3
    // levels of quantisation; geometry (which pixels, which triangle, the
    // flat bands) is exact.
    const u64 clamp = square(4);
    const u64 ci = fill(clamp, 30, 24, {0, 0, 30, 24});
    expect_row(ci, "pgfill sq clamp y=10:", 10, false, 3);
    expect_row(ci, "pgfill sq clamp y=3:", 3, false, 3);
    expect_row(ci, "pgfill sq clamp y=22:", 22, false, 0);
    expect_row(ci, "pgfill sq clamp x=10:", 10, true, 3);
    const u64 ti = fill(square(0), 30, 24, {0, 0, 30, 24});
    expect_row(ti, "pgfill sq tile y=10:", 10, false, 3);
    expect_row(ti, "pgfill sq tile y=22:", 22, false, 3);
    expect_row(fill(square(1), 30, 24, {0, 0, 30, 24}), "pgfill sq flipx y=10:", 10, false, 3);
    brush(clamp)->gamma = 1;
    expect_row(fill(clamp, 30, 24, {0, 0, 30, 24}), "pgfill sq gamma y=10:", 10, false, 4);
    brush(clamp)->gamma = 0;
    brush(clamp)->focus = {0.5f, 0.5f};
    expect_row(fill(clamp, 30, 24, {0, 0, 30, 24}), "pgfill sq focus.5 y=10:", 10, false, 3);
    brush(clamp)->focus = {0, 0};
    ASSERT_EQ(call("GdipSetPathGradientSigmaBlend", {I(clamp), R(0.5f), R(1)}), Status::Ok);
    expect_row(fill(clamp, 30, 24, {0, 0, 30, 24}), "pgfill sq sigma y=10:", 10, false, 6);
    write_ints(BUF + 0x100, {static_cast<i32>(0xff00ff00), static_cast<i32>(0xffffffff), static_cast<i32>(0xff000000)});
    write_reals(BUF + 0x140, {0, 0.4f, 1});
    ASSERT_EQ(call("GdipSetPathGradientPresetBlend", {I(clamp), I(BUF + 0x100), I(BUF + 0x140), I(3)}), Status::Ok);
    expect_row(fill(clamp, 30, 24, {0, 0, 30, 24}), "pgfill sq preset y=10:", 10, false, 5);
    const u64 off = square(4);
    write_reals(OUT3, {5, 5});
    ASSERT_EQ(call("GdipSetPathGradientCenterPoint", {I(off), I(OUT3)}), Status::Ok);
    const u64 oi = fill(off, 30, 24, {0, 0, 30, 24});
    expect_row(oi, "pgfill sq center5,5 y=5:", 5, false, 3);
    expect_row(oi, "pgfill sq center5,5 y=12:", 12, false, 3);
    // Per-vertex surround colours.
    const u64 four = make_brush("GdipCreatePathGradient", {I(BUF), I(4), I(4), I(OUT)});
    write_ints(BUF + 0x100, {static_cast<i32>(0xffff0000), static_cast<i32>(0xff00ff00), static_cast<i32>(0xff0000ff),
                             static_cast<i32>(0xff000000)});
    write_int(OUT2, 4);
    ASSERT_EQ(call("GdipSetPathGradientSurroundColorsWithCount", {I(four), I(BUF + 0x100), I(OUT2)}), Status::Ok);
    ASSERT_EQ(call("GdipSetPathGradientCenterColor", {I(four), I(0xffffffff)}), Status::Ok);
    const u64 fi = fill(four, 30, 24, {0, 0, 30, 24});
    for (i32 y : {0, 5, 19}) expect_row(fi, "pgfill sq 4colors y=" + std::to_string(y) + ":", y, false, 3);
    expect_row(fi, "pgfill sq 4colors x=0:", 0, true, 3);
    // Non-uniform focus, one blend factor.
    const u64 nf = square(4);
    brush(nf)->focus = {0.5f, 0.25f};
    const u64 nfi = fill(nf, 24, 24, {0, 0, 24, 24});
    expect_row(nfi, "pgfill focus.5,.25 y=10:", 10, false, 3);
    expect_row(nfi, "pgfill focus.5,.25 x=10:", 10, true, 3);
    expect_row(nfi, "pgfill focus.5,.25 y=4:", 4, false, 3);
    const u64 b1 = square(4);
    write_reals(BUF + 0x180, {0.5f, 0});
    ASSERT_EQ(call("GdipSetPathGradientBlend", {I(b1), I(BUF + 0x180), I(BUF + 0x184), I(1)}), Status::Ok);
    expect_row(fill(b1, 24, 24, {0, 0, 24, 24}), "pgfill blend1 .5 y=10:", 10, false, 3);
    // The fan, not the polygon interior, is painted ("pgfill concave").
    write_reals(BUF + 0x200, {0, 0, 20, 0, 20, 20, 14, 20, 10, 4, 6, 20});
    const u64 cav = make_brush("GdipCreatePathGradient", {I(BUF + 0x200), I(6), I(4), I(OUT)});
    ASSERT_EQ(call("GdipSetPathGradientCenterColor", {I(cav), I(0xffff0000)}), Status::Ok);
    write_int(OUT2, 1);
    ASSERT_EQ(call("GdipSetPathGradientSurroundColorsWithCount", {I(cav), I(BUF + 0x80), I(OUT2)}), Status::Ok);
    const u64 cavi = fill(cav, 24, 24, {0, 0, 24, 24});
    for (i32 y : {2, 12, 18}) expect_row(cavi, "pgfill concave y=" + std::to_string(y) + ":", y, false, 3);
    // Figures are joined into one polygon ("pgfill 2rects").
    GpPath path;
    for (auto [x, y] : {std::pair{0.f, 0.f}, {8.f, 0.f}, {8.f, 8.f}, {0.f, 8.f}, {12.f, 0.f}, {20.f, 0.f}, {20.f, 8.f}, {12.f, 8.f}}) {
        path.points.push_back({x, y});
        path.types.push_back(path.types.size() % 4 == 0 ? 0 : (path.types.size() % 4 == 3 ? 0x81 : 1));
    }
    const u64 two = make_brush("GdipCreatePathGradientFromPath", {I(mgr.objects().add(path)), I(OUT)});
    ASSERT_EQ(call("GdipSetPathGradientCenterColor", {I(two), I(0xffff0000)}), Status::Ok);
    write_int(OUT2, 1);
    ASSERT_EQ(call("GdipSetPathGradientSurroundColorsWithCount", {I(two), I(BUF + 0x80), I(OUT2)}), Status::Ok);
    expect_row(fill(two, 24, 24, {0, 0, 24, 24}), "pgfill 2rects y=4:", 4, false, 3);
    // Brush transform moves the gradient ("pgfill tri xlate4,2").
    write_reals(BUF + 0x200, {0, 0, 20, 0, 0, 20});
    const u64 tri = make_brush("GdipCreatePathGradient", {I(BUF + 0x200), I(3), I(4), I(OUT)});
    ASSERT_EQ(call("GdipSetPathGradientCenterColor", {I(tri), I(0xff000000)}), Status::Ok);
    write_ints(BUF + 0x100, {static_cast<i32>(0xffff0000), static_cast<i32>(0xff00ff00), static_cast<i32>(0xff0000ff)});
    write_int(OUT2, 3);
    ASSERT_EQ(call("GdipSetPathGradientSurroundColorsWithCount", {I(tri), I(BUF + 0x100), I(OUT2)}), Status::Ok);
    // Interior colours of a multi-colour triangle deviate near the centre
    // (contract doc); the boundary pixel and the painted extent match.
    const u64 trii = fill(tri, 30, 24, {0, 0, 30, 24});
    expect_row(trii, "pgfill tri y=6:", 6, false, 1, 0, 1);
    expect_row(trii, "pgfill tri y=6:", 6, false, 0, 14, 2);
    ASSERT_EQ(call("GdipTranslatePathGradientTransform", {I(tri), R(4), R(2), I(0)}), Status::Ok);
    const u64 trx = fill(tri, 30, 24, {0, 0, 30, 24});
    expect_row(trx, "pgfill tri xlate4,2 y=3:", 3, false, 6, 0, 17);
    expect_row(trx, "pgfill tri xlate4,2 y=3:", 3, false, 0, 23, 2);
}

}  // namespace

INSTANTIATE_GDIP_BITNESS(Gdip_pen_brush_Test);

}  // namespace vitreo::winapi::gdip_test
