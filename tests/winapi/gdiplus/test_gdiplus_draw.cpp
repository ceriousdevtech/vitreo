// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus_draw.cpp
/// @brief Contract tests for the GDI+ draw area (gdiplus_draw.cpp,
/// gdiplus_pen.cpp), run in both bitnesses through gdiplus_fixture.hpp.
/// Expected values come from tools/oracle_probe/gdiplus_draw_probe.c run
/// on winoracle (gdiplus_draw_probe.expected / .x86.expected).

#include "gdiplus_fixture.hpp"

#include <winapi/graphics/gdiplus/gdiplus_geometry.hpp>
#include <winapi/graphics/gdiplus/gdiplus_graphics.hpp>
#include <winapi/graphics/gdiplus/gdiplus_image.hpp>
#include <winapi/graphics/gdiplus/gdiplus_pen.hpp>
#include <winapi/graphics/gdiplus/gdiplus_pen_brush.hpp>

#include <algorithm>
#include <cmath>

namespace vitreo::winapi::gdip_test {
namespace {

using namespace gdip;

class Gdip_draw_Test : public GdipTest {
protected:
    u64 pen(u32 argb = 0xFF000000, float width = 1, i32 unit = UnitWorld) {
        poison(OUT);
        EXPECT_EQ(call("GdipCreatePen1", {I(argb), R(width), I(static_cast<u64>(unit)), I(OUT)}),
                  Status::Ok);
        return read_handle(OUT);
    }
    GpPen* pen_obj(u64 h) { return mgr.objects().get<GpPen>(h); }
    u64 brush(u32 argb) {
        GpBrush b;
        b.type = BrushTypeSolidColor;
        b.color = argb;
        return mgr.objects().add(b);
    }
    u64 matrix(Affine a) { return mgr.objects().add(GpMatrix{a}); }
    u64 path(std::vector<PointF> pts, bool closed) {
        GpPath p;
        for (usize i = 0; i < pts.size(); ++i) {
            p.points.push_back(pts[i]);
            p.types.push_back(i == 0 ? PathPointTypeStart : PathPointTypeLine);
        }
        if (closed) p.types.back() |= PathPointTypeCloseSubpath;
        return mgr.objects().add(p);
    }
    /// A 64x64 32bppARGB bitmap of @p fill and a graphics on it.
    void canvas(u32 fill = 0) {
        GpImage img;
        img.width = 64;
        img.height = 64;
        img.pixel_format = PixelFormat32bppARGB;
        img.stride = 256;
        img.bits.resize(256 * 64);
        for (usize i = 0; i < img.bits.size(); i += 4) std::memcpy(&img.bits[i], &fill, 4);
        image = mgr.objects().add(img);
        GpGraphics g;
        g.kind = TargetKind::Image;
        g.image = image;
        graphics = mgr.objects().add(g);
    }
    GpGraphics& gfx() { return *mgr.objects().get<GpGraphics>(graphics); }
    u32 px(int x, int y) {
        const GpImage* img = mgr.objects().get<GpImage>(image);
        u32 v = 0;
        std::memcpy(&v, &img->bits[static_cast<usize>(y) * 256 + static_cast<usize>(x) * 4], 4);
        return v;
    }
    u32 alpha(int x, int y) { return px(x, y) >> 24; }
    int count_px() {
        int n = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) n += alpha(x, y) != 0;
        return n;
    }
    Status line(u64 p, float x1, float y1, float x2, float y2) {
        return call("GdipDrawLine", {I(graphics), I(p), R(x1), R(y1), R(x2), R(y2)});
    }

    u64 image{0};
    u64 graphics{0};
};

// ---- Widened geometry (widen_path, used by GdipWidenPath) ------------------

struct Box { float x0, y0, x1, y1; };
Box bounds(const GpPath& p) {
    Box b{1e9f, 1e9f, -1e9f, -1e9f};
    for (PointF q : p.points) {
        b.x0 = std::min(b.x0, q.X); b.y0 = std::min(b.y0, q.Y);
        b.x1 = std::max(b.x1, q.X); b.y1 = std::max(b.y1, q.Y);
    }
    return b;
}
GpPath poly(std::vector<PointF> pts, bool closed = false) {
    GpPath p;
    for (usize i = 0; i < pts.size(); ++i) {
        p.points.push_back(pts[i]);
        p.types.push_back(i == 0 ? PathPointTypeStart : PathPointTypeLine);
    }
    if (closed) p.types.back() |= PathPointTypeCloseSubpath;
    return p;
}
GpPath widened(const GpPath& in, const GpPen& pen) {
    GpPath out;
    widen_path(in, pen, nullptr, 1.0f, out);
    return out;
}
GpPen wpen(float w) {
    GpPen p;
    p.width = w;
    p.unit = UnitWorld;
    p.brush = std::make_shared<GpBrush>();
    return p;
}
int figures(const GpPath& p) {
    int n = 0;
    for (u8 t : p.types) n += (t & PathPointTypePathTypeMask) == PathPointTypeStart;
    return n;
}
void expect_points(const GpPath& p, std::vector<PointF> want, std::vector<u8> types) {
    ASSERT_EQ(p.points.size(), want.size());
    for (usize i = 0; i < want.size(); ++i) {
        EXPECT_NEAR(p.points[i].X, want[i].X, 2e-3f) << "point " << i;
        EXPECT_NEAR(p.points[i].Y, want[i].Y, 2e-3f) << "point " << i;
        EXPECT_EQ(p.types[i], types[i]) << "type " << i;
    }
}
void expect_box(const GpPath& p, Box want) {
    const Box b = bounds(p);
    EXPECT_NEAR(b.x0, want.x0, 2e-3f);
    EXPECT_NEAR(b.y0, want.y0, 2e-3f);
    EXPECT_NEAR(b.x1, want.x1, 2e-3f);
    EXPECT_NEAR(b.y1, want.y1, 2e-3f);
}

TEST(Gdip_draw_Widen, CapsMatchOracle) {
    const GpPath line = poly({{10, 10}, {30, 10}});
    GpPen p = wpen(4);
    GpPath out = widened(line, p);
    EXPECT_EQ(out.fill_mode, FillModeWinding);
    expect_points(out, {{10, 8}, {30, 8}, {30, 12}, {10, 12}}, {0, 1, 1, 0x81});
    p.start_cap = LineCapSquare;
    expect_points(widened(line, p), {{8, 8}, {30, 8}, {30, 12}, {8, 12}}, {0, 1, 1, 0x81});
    p.start_cap = LineCapTriangle;
    expect_points(widened(line, p), {{10, 8}, {30, 8}, {30, 12}, {10, 12}, {8, 10}, {10, 8}},
                  {0, 1, 1, 1, 1, 0x81});
    p.start_cap = LineCapRound;
    expect_points(widened(line, p),
                  {{10, 8}, {30, 8}, {30, 12}, {10, 12}, {8.895f, 12}, {8, 11.105f}, {8, 10},
                   {8, 8.895f}, {8.895f, 8}, {10, 8}},
                  {0, 1, 1, 1, 3, 3, 3, 3, 3, 0x83});
    // Anchors: separate figures sized by max(width, 2) (probe startcap-0x1*, r2 w*-endcap).
    p.start_cap = LineCapSquareAnchor;
    expect_box(widened(line, p), {7.17157f, 7.17157f, 30, 12.82843f});
    p.start_cap = LineCapRoundAnchor;
    expect_box(widened(line, p), {6, 6, 30, 14});
    p.start_cap = LineCapDiamondAnchor;
    expect_box(widened(line, p), {6, 6, 30, 14});
    p.start_cap = LineCapArrowAnchor;
    out = widened(line, p);
    expect_points(out, {{14, 8}, {30, 8}, {30, 12}, {14, 12}, {10, 10}, {16.928f, 6}, {16.928f, 14}},
                  {0, 1, 1, 0x81, 0, 1, 0x81});
    GpPen thin = wpen(1);
    thin.end_cap = LineCapArrowAnchor;
    expect_points(widened(line, thin),
                  {{10, 9.5f}, {29, 9.5f}, {29, 10.5f}, {10, 10.5f}, {30, 10}, {26.536f, 12},
                   {26.536f, 8}},
                  {0, 1, 1, 0x81, 0, 1, 0x81});
    thin.end_cap = LineCapRound;
    expect_points(widened(line, thin),
                  {{10, 9.5f}, {30, 9.5f}, {30.276f, 9.5f}, {30.5f, 9.724f}, {30.5f, 10},
                   {30.5f, 10.276f}, {30.276f, 10.5f}, {30, 10.5f}, {10, 10.5f}},
                  {0, 1, 3, 3, 3, 3, 3, 3, 0x81});
    GpPen w8 = wpen(8);
    w8.end_cap = LineCapSquareAnchor;
    expect_box(widened(line, w8), {10, 4.34315f, 35.65685f, 15.65685f});
}

TEST(Gdip_draw_Widen, JoinsMatchOracle) {
    const GpPath vee = poly({{0, 0}, {20, 0}, {0, 4}});
    const GpPath right = poly({{0, 0}, {20, 0}, {20, 20}});
    GpPen p = wpen(2);
    // Miter beyond the limit is clipped at limit*w/2 from the vertex.
    expect_points(widened(vee, p),
                  {{0, -1}, {29.950f, -1}, {29.953f, -0.971f}, {0.196f, 4.981f},
                   {-0.196f, 3.019f}, {19.804f, -0.981f}, {20, 1}, {0, 1}},
                  {0, 1, 1, 1, 1, 1, 1, 0x81});
    expect_points(widened(right, p),
                  {{0, -1}, {21, -1}, {21, 20}, {19, 20}, {19, 0}, {20, 1}, {0, 1}},
                  {0, 1, 1, 1, 1, 1, 0x81});
    p.miter_limit = 100;
    expect_box(widened(vee, p), {-0.196116f, -1, 30.0990f, 4.98058f});
    p.miter_limit = 10;
    p.line_join = LineJoinMiterClipped;
    expect_box(widened(vee, p), {-0.196116f, -1, 20.1961f, 4.98058f});
    p.line_join = LineJoinBevel;
    expect_points(widened(right, p),
                  {{0, -1}, {20, -1}, {21, 0}, {21, 20}, {19, 20}, {19, 0}, {20, 1}, {0, 1}},
                  {0, 1, 1, 1, 1, 1, 1, 0x81});
    p.line_join = LineJoinRound;
    expect_points(widened(right, p),
                  {{0, -1}, {20, -1}, {20.552f, -1}, {21, -0.552f}, {21, 0}, {21, 20}, {19, 20},
                   {19, 0}, {20, 1}, {0, 1}},
                  {0, 1, 3, 3, 3, 1, 1, 1, 1, 0x81});
    expect_box(widened(vee, p), {-0.196116f, -1, 21.0459f, 4.98058f});
}

TEST(Gdip_draw_Widen, DashesMatchOracle) {
    const GpPath l40 = poly({{0, 0}, {40, 0}});
    GpPen p = wpen(2);
    p.dash_style = DashStyleDash;
    p.dash_array = dash_pattern(DashStyleDash);
    GpPath out = widened(l40, p);
    EXPECT_EQ(figures(out), 5);
    expect_box(out, {0, -1, 38, 1});
    EXPECT_NEAR(out.points[4].X, 8, 1e-3f);
    p.dash_offset = 1;  // in pen widths
    out = widened(l40, p);
    EXPECT_EQ(figures(out), 6);
    EXPECT_NEAR(out.points[1].X, 4, 1e-3f);
    EXPECT_NEAR(out.points[4].X, 6, 1e-3f);
    p.dash_offset = 0;
    p.dash_cap = DashCapTriangle;  // dash body shortened by the width, tips at w/2
    out = widened(l40, p);
    expect_box(out, {0, -1, 37, 1});
    EXPECT_NEAR(out.points[2].X, 5, 1e-3f);
    EXPECT_NEAR(out.points[10].X, 7, 1e-3f);
    p.dash_cap = DashCapFlat;
    p.end_cap = LineCapSquare;  // only a dash touching the end gets the end cap
    expect_box(widened(poly({{0, 0}, {38, 0}}), p), {0, -1, 39, 1});
    expect_box(widened(l40, p), {0, -1, 38, 1});
    // Thin pens dash in units of max(width, 1) (probe dash1-w0.5).
    GpPen thin = wpen(0.5f);
    thin.dash_array = dash_pattern(DashStyleDash);
    out = widened(l40, thin);
    EXPECT_EQ(figures(out), 10);
    EXPECT_NEAR(out.points[1].X, 3, 1e-3f);
    // Closed figures start dashing at the last point, along the closing
    // segment (probe closed-dash-w2: first dash on the left edge y 10..4).
    const GpPath sq = poly({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, true);
    out = widened(sq, p);
    EXPECT_EQ(figures(out), 5);
    expect_points(GpPath{{out.points.begin(), out.points.begin() + 4}, {0, 1, 1, 0x81}},
                  {{-1, 10}, {-1, 4}, {1, 4}, {1, 10}}, {0, 1, 1, 0x81});
}

TEST(Gdip_draw_Widen, AlignmentCompoundTransformUnit) {
    const GpPath sq = poly({{10, 10}, {30, 10}, {30, 30}, {10, 30}}, true);
    GpPen p = wpen(4);
    GpPath out = widened(sq, p);
    EXPECT_EQ(figures(out), 2);
    expect_box(out, {8, 8, 32, 32});
    p.alignment = PenAlignmentInset;
    out = widened(sq, p);
    expect_box(out, {10, 10, 30, 30});
    // The inner loop lies 4 inside.
    bool inner = false;
    for (PointF q : out.points) inner |= std::fabs(q.X - 14) < 1e-3f && std::fabs(q.Y - 14) < 1e-3f;
    EXPECT_TRUE(inner);
    // Counter-clockwise input insets the same way.
    expect_box(widened(poly({{10, 10}, {10, 30}, {30, 30}, {30, 10}}, true), p), {10, 10, 30, 30});
    // Open figures ignore Inset.
    expect_box(widened(poly({{10, 10}, {30, 10}, {30, 30}, {10, 30}}), p), {10, 8, 32, 32});
    p.alignment = PenAlignmentCenter;
    p.compound = {0, 0.25f, 0.75f, 1};
    expect_points(widened(poly({{10, 10}, {30, 10}}), p),
                  {{10, 8}, {30, 8}, {30, 9}, {10, 9}, {10, 11}, {30, 11}, {30, 12}, {10, 12}},
                  {0, 1, 1, 0x81, 0, 1, 1, 0x81});
    out = widened(sq, p);
    EXPECT_EQ(figures(out), 4);
    expect_box(out, {8, 8, 32, 32});
    // Pen transform scales the nib (probe vline-penxform-3x1).
    GpPen t = wpen(2);
    t.transform = Affine{3, 0, 0, 1, 0, 0};
    expect_box(widened(poly({{10, 10}, {30, 10}}), t), {10, 9, 30, 11});
    expect_box(widened(poly({{10, 10}, {10, 30}}), t), {7, 10, 13, 30});
    // Width 0 widens one unit; physical units widen to nothing.
    expect_box(widened(poly({{10, 10}, {30, 10}}), wpen(0)), {10, 9.5f, 30, 10.5f});
    GpPen pt = wpen(3);
    pt.unit = UnitPoint;
    EXPECT_TRUE(widened(poly({{10, 10}, {30, 10}}), pt).points.empty());
}

TEST(Gdip_draw_Widen, CustomAndArrowCaps) {
    const GpPath line = poly({{10, 10}, {30, 10}});
    GpPen p = wpen(2);
    auto arrow = std::make_shared<GpCustomLineCap>();
    arrow->type = CustomLineCapTypeAdjustableArrow;
    arrow->arrow_height = 3;
    arrow->arrow_width = 2;
    arrow->base_inset = 1.5f;
    p.end_cap = LineCapCustom;
    p.custom_end = arrow;
    expect_points(widened(line, p),
                  {{10, 9}, {27, 9}, {27, 11}, {10, 11}, {24, 8}, {30, 10}, {24, 12}},
                  {0, 1, 1, 0x81, 0, 1, 0x81});
    arrow->arrow_middle_inset = 1;
    expect_points(widened(line, p),
                  {{10, 9}, {27, 9}, {27, 11}, {10, 11}, {24, 8}, {30, 10}, {24, 12}, {26, 10}},
                  {0, 1, 1, 0x81, 0, 1, 1, 0x81});
    auto fc = std::make_shared<GpCustomLineCap>();
    fc->fill_path = std::make_shared<GpPath>(poly({{-1, -2}, {1, -2}, {0, 0}}, true));
    fc->base_inset = 1;
    p.custom_end = fc;
    p.start_cap = LineCapCustom;
    p.custom_start = fc;
    expect_points(widened(line, p),
                  {{12, 9}, {28, 9}, {28, 11}, {12, 11}, {14, 8}, {14, 12}, {10, 10}, {26, 12},
                   {26, 8}, {30, 10}},
                  {0, 1, 1, 0x81, 0, 1, 0x81, 0, 1, 0x81});
    fc->width_scale = 2;
    p.start_cap = LineCapFlat;
    p.custom_start.reset();
    expect_box(widened(line, p), {10, 6, 30, 14});
    auto sc = std::make_shared<GpCustomLineCap>();
    sc->stroke_path = std::make_shared<GpPath>(poly({{-1, -2}, {1, -2}, {0, 0}}));
    sc->base_inset = 1;
    p.custom_end = sc;
    expect_box(widened(line, p), {10, 6.38197f, 30.4472f, 12});
}

// ---- Pens -------------------------------------------------------------------

TEST_P(Gdip_draw_Test, PenDefaultsAndCreateValidation) {
    const u64 p = pen(0xFF112233, 2.5f, UnitWorld);
    ASSERT_NE(p, 0u);
    poison(OUT2, 4);
    EXPECT_EQ(call("GdipGetPenWidth", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 2.5f);
    EXPECT_EQ(call("GdipGetPenUnit", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    for (const char* name : {"GdipGetPenStartCap", "GdipGetPenEndCap", "GdipGetPenDashCap197819",
                             "GdipGetPenLineJoin", "GdipGetPenMode", "GdipGetPenDashStyle",
                             "GdipGetPenDashCount", "GdipGetPenCompoundCount",
                             "GdipGetPenFillType"}) {
        poison(OUT2, 4);
        EXPECT_EQ(call(name, {I(p), I(OUT2)}), Status::Ok) << name;
        EXPECT_EQ(read_int(OUT2), 0) << name;
    }
    EXPECT_EQ(call("GdipGetPenMiterLimit", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 10.0f);
    EXPECT_EQ(call("GdipGetPenDashOffset", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 0.0f);
    EXPECT_EQ(call("GdipGetPenColor", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(static_cast<u32>(read_int(OUT2)), 0xFF112233u);
    const u64 mx = matrix(Affine{9, 9, 9, 9, 9, 9});
    EXPECT_EQ(call("GdipGetPenTransform", {I(p), I(mx)}), Status::Ok);
    EXPECT_TRUE(mgr.objects().get<GpMatrix>(mx)->m.is_identity());

    for (i32 u : {-1, 1, 7}) {
        EXPECT_EQ(call("GdipCreatePen1", {I(0xFF000000), R(1), I(static_cast<u64>(u)), I(OUT)}),
                  Status::InvalidParameter);
    }
    EXPECT_EQ(call("GdipCreatePen1", {I(0), R(1), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(pen_obj(pen(0, -1))->width, 1.0f);
    EXPECT_EQ(pen_obj(pen(0, 0))->width, 0.0f);
    EXPECT_EQ(call("GdipCreatePen2", {I(0), R(1), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeletePen", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeletePen", {I(p)}), Status::Ok);
    EXPECT_EQ(pen_obj(p), nullptr);
}

TEST_P(Gdip_draw_Test, PenSettersStoreAsWindows) {
    const u64 p = pen();
    GpPen* o = pen_obj(p);
    EXPECT_EQ(call("GdipSetPenWidth", {I(p), R(-1)}), Status::Ok);
    EXPECT_EQ(o->width, -1.0f);
    EXPECT_EQ(call("GdipSetPenUnit", {I(p), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPenUnit", {I(p), I(7)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPenUnit", {I(p), I(6)}), Status::Ok);
    EXPECT_EQ(o->unit, 6);
    for (u32 cap : {0xFFFFFFFFu, 4u, 0x15u, 0xF0u, 0xFFu, 0x100u}) {
        EXPECT_EQ(call("GdipSetPenStartCap", {I(p), I(cap)}), Status::Ok);
        EXPECT_EQ(static_cast<u32>(o->start_cap), cap);
    }
    for (auto [in, got] : std::vector<std::pair<i32, i32>>{{-1, 0}, {1, 0}, {2, 2}, {3, 3}, {4, 0}}) {
        EXPECT_EQ(call("GdipSetPenDashCap197819", {I(p), I(static_cast<u64>(in))}), Status::Ok);
        EXPECT_EQ(o->dash_cap, got) << in;
    }
    EXPECT_EQ(call("GdipSetPenLineCap197819", {I(p), I(2), I(0x14), I(3)}), Status::Ok);
    EXPECT_EQ(o->start_cap, 2);
    EXPECT_EQ(o->end_cap, 0x14);
    EXPECT_EQ(o->dash_cap, 3);
    EXPECT_EQ(call("GdipSetPenLineCap197819", {I(p), I(1), I(1), I(1)}), Status::Ok);
    EXPECT_EQ(o->dash_cap, 0);
    EXPECT_EQ(call("GdipSetPenLineJoin", {I(p), I(4)}), Status::Ok);
    EXPECT_EQ(o->line_join, 4);
    for (auto [in, got] : std::vector<std::pair<float, float>>{{-1, 1}, {0, 1}, {0.5f, 1}, {3, 3}}) {
        EXPECT_EQ(call("GdipSetPenMiterLimit", {I(p), R(in)}), Status::Ok);
        EXPECT_EQ(o->miter_limit, got);
    }
    EXPECT_EQ(call("GdipSetPenMode", {I(p), I(5)}), Status::Ok);
    EXPECT_EQ(o->alignment, 5);
    EXPECT_EQ(call("GdipGetPenWidth", {I(p), I(0)}), Status::InvalidParameter);
    // Another class's object where a pen belongs: ObjectBusy.
    EXPECT_EQ(call("GdipSetPenWidth", {I(brush(0)), R(1)}), Status::ObjectBusy);
}

TEST_P(Gdip_draw_Test, PenDashArrays) {
    const u64 p = pen();
    GpPen* o = pen_obj(p);
    EXPECT_EQ(call("GdipGetPenDashArray", {I(p), I(BUF), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPenDashStyle", {I(p), I(4)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPenDashCount", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 6);
    EXPECT_EQ(call("GdipGetPenDashArray", {I(p), I(BUF), I(6)}), Status::Ok);
    EXPECT_EQ(read_real(BUF), 3.0f);
    EXPECT_EQ(read_real(BUF + 20), 1.0f);
    EXPECT_EQ(call("GdipSetPenDashStyle", {I(p), I(5)}), Status::Ok);  // keeps the array
    EXPECT_EQ(o->dash_array.size(), 6u);
    EXPECT_EQ(call("GdipSetPenDashStyle", {I(p), I(1)}), Status::Ok);
    EXPECT_EQ(call("GdipSetPenDashStyle", {I(p), I(6)}), Status::Ok);  // ignored
    EXPECT_EQ(o->dash_style, 1);
    write_reals(BUF, {2, 1, 0.5f});
    EXPECT_EQ(call("GdipSetPenDashArray", {I(p), I(BUF), I(3)}), Status::Ok);
    EXPECT_EQ(o->dash_style, DashStyleCustom);
    poison(BUF + 0x100, 16);
    EXPECT_EQ(call("GdipGetPenDashArray", {I(p), I(BUF + 0x100), I(2)}), Status::Ok);
    EXPECT_EQ(read_real(BUF + 0x104), 1.0f);
    EXPECT_EQ(memory.read_u32(BUF + 0x108).value(), 0xCCCCCCCCu);
    EXPECT_EQ(call("GdipGetPenDashArray", {I(p), I(BUF + 0x100), I(4)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetPenDashArray", {I(p), I(BUF + 0x100), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPenDashArray", {I(p), I(0), I(3)}), Status::InvalidParameter);
    write_reals(BUF, {0, 1});
    EXPECT_EQ(call("GdipSetPenDashArray", {I(p), I(BUF), I(2)}), Status::InvalidParameter);
    write_reals(BUF, {-1, 1});
    EXPECT_EQ(call("GdipSetPenDashArray", {I(p), I(BUF), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPenDashArray", {I(p), I(BUF), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPenDashArray", {I(p), I(0), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(o->dash_array.size(), 3u);
    EXPECT_EQ(call("GdipSetPenDashOffset", {I(p), R(-2)}), Status::Ok);
    EXPECT_EQ(call("GdipSetPenDashStyle", {I(p), I(0)}), Status::Ok);
    EXPECT_TRUE(o->dash_array.empty());
    EXPECT_EQ(o->dash_offset, -2.0f);  // offset survives Solid
}

TEST_P(Gdip_draw_Test, PenCompoundArrays) {
    const u64 p = pen();
    GpPen* o = pen_obj(p);
    EXPECT_EQ(call("GdipGetPenCompoundArray", {I(p), I(BUF), I(1)}), Status::InvalidParameter);
    write_reals(BUF, {0, 0.25f, 0.75f, 1});
    EXPECT_EQ(call("GdipSetPenCompoundArray", {I(p), I(BUF), I(4)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPenCompoundArray", {I(p), I(BUF + 0x100), I(3)}), Status::Ok);
    EXPECT_EQ(read_real(BUF + 0x108), 0.75f);
    EXPECT_EQ(call("GdipGetPenCompoundArray", {I(p), I(BUF + 0x100), I(5)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetPenCompoundArray", {I(p), I(BUF + 0x100), I(0)}), Status::Ok);
    auto set = [&](std::initializer_list<float> v, i32 n) {
        write_reals(BUF, v);
        return call("GdipSetPenCompoundArray", {I(p), I(BUF), I(static_cast<u64>(n))});
    };
    EXPECT_EQ(set({0, 0.5f, 1}, 3), Status::InvalidParameter);
    EXPECT_EQ(set({0, 0.75f, 0.25f, 1}, 4), Status::InvalidParameter);
    EXPECT_EQ(set({0, 0.5f, 0.75f, 1.5f}, 4), Status::InvalidParameter);
    EXPECT_EQ(set({-0.5f, 0.5f}, 2), Status::InvalidParameter);
    EXPECT_EQ(set({0.5f}, 1), Status::InvalidParameter);
    EXPECT_EQ(set({0.25f, 0.75f}, 0), Status::InvalidParameter);
    EXPECT_EQ(set({0, 0.5f, 0.5f, 1}, 4), Status::Ok);
    EXPECT_EQ(set({0.25f, 0.75f}, 2), Status::Ok);
    EXPECT_EQ(o->compound.size(), 2u);
    EXPECT_EQ(call("GdipSetPenCompoundArray", {I(p), I(0), I(2)}), Status::InvalidParameter);
}

TEST_P(Gdip_draw_Test, PenTransformOps) {
    const u64 p = pen();
    const u64 m = matrix(Affine{2, 0, 0, 3, 5, 6});
    const u64 sing = matrix(Affine{1, 1, 1, 1, 0, 0});
    EXPECT_EQ(call("GdipSetPenTransform", {I(p), I(m)}), Status::Ok);
    EXPECT_EQ(call("GdipSetPenTransform", {I(p), I(sing)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipRotatePenTransform", {I(p), R(90), I(1)}), Status::Ok);
    EXPECT_EQ(call("GdipTranslatePenTransform", {I(p), R(1), R(2), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipScalePenTransform", {I(p), R(2), R(2), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipMultiplyPenTransform", {I(p), I(m), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipMultiplyPenTransform", {I(p), I(sing), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipMultiplyPenTransform", {I(p), I(0), I(0)}), Status::Ok);
    const Affine t = pen_obj(p)->transform;  // probe: 0,4,-9,0,-30,17
    EXPECT_NEAR(t.m11, 0, 1e-5f);
    EXPECT_NEAR(t.m12, 4, 1e-5f);
    EXPECT_NEAR(t.m21, -9, 1e-5f);
    EXPECT_NEAR(t.m22, 0, 1e-5f);
    EXPECT_NEAR(t.dx, -30, 1e-4f);
    EXPECT_NEAR(t.dy, 17, 1e-4f);
    EXPECT_EQ(call("GdipResetPenTransform", {I(p)}), Status::Ok);
    EXPECT_TRUE(pen_obj(p)->transform.is_identity());
    EXPECT_EQ(call("GdipGetPenTransform", {I(p), I(0)}), Status::InvalidParameter);
}

TEST_P(Gdip_draw_Test, PenBrushFillAndClone) {
    GpBrush hatch;
    hatch.type = BrushTypeHatchFill;
    const u64 hb = mgr.objects().add(hatch);
    const u64 sb = brush(0x80FF0000);
    poison(OUT);
    ASSERT_EQ(call("GdipCreatePen2", {I(sb), R(3), I(2), I(OUT)}), Status::Ok);
    const u64 p = read_handle(OUT);
    EXPECT_EQ(call("GdipGetPenColor", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0x80FF0000u);
    poison(OUT3);
    EXPECT_EQ(call("GdipGetPenBrushFill", {I(p), I(OUT3)}), Status::Ok);
    const u64 got = read_handle(OUT3);
    EXPECT_NE(got, sb);
    ASSERT_NE(mgr.objects().get<GpBrush>(got), nullptr);
    EXPECT_EQ(mgr.objects().get<GpBrush>(got)->color, 0x80FF0000u);
    EXPECT_EQ(call("GdipSetPenBrushFill", {I(p), I(hb)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPenFillType", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
    write_int(OUT2, 0x12345678);
    EXPECT_EQ(call("GdipGetPenColor", {I(p), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPenColor", {I(p), I(0xFF0000FF)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPenFillType", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("GdipSetPenBrushFill", {I(p), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetPenBrushFill", {I(p), I(0)}), Status::InvalidParameter);
    poison(OUT3);
    EXPECT_EQ(call("GdipClonePen", {I(p), I(OUT3)}), Status::Ok);
    const u64 c = read_handle(OUT3);
    EXPECT_NE(c, p);
    EXPECT_EQ(call("GdipSetPenColor", {I(c), I(0xFF00FF00)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPenColor", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0xFF0000FFu);
    EXPECT_EQ(call("GdipClonePen", {I(p), I(0)}), Status::InvalidParameter);
}

TEST_P(Gdip_draw_Test, CustomLineCaps) {
    const u64 above = path({{-1, 0}, {1, 0}, {0, 1}}, true);
    const u64 negy = path({{-1, -2}, {1, -2}, {0, 0}}, true);
    const u64 stroke = path({{-1, 0}, {1, 0}, {0, 1}}, false);
    EXPECT_EQ(call("GdipCreateCustomLineCap", {I(0), I(0), I(0), R(0), I(OUT)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateCustomLineCap", {I(above), I(0), I(0), R(0), I(OUT)}),
              Status::NotImplemented);
    EXPECT_EQ(call("GdipCreateCustomLineCap", {I(above), I(stroke), I(0), R(0), I(OUT)}),
              Status::NotImplemented);
    EXPECT_EQ(call("GdipCreateCustomLineCap", {I(negy), I(0), I(0), R(0), I(0)}),
              Status::InvalidParameter);
    poison(OUT);
    ASSERT_EQ(call("GdipCreateCustomLineCap", {I(negy), I(0), I(0), R(1), I(OUT)}), Status::Ok);
    const u64 cap = read_handle(OUT);
    poison(OUT2);
    ASSERT_EQ(call("GdipCreateCustomLineCap", {I(0), I(stroke), I(0), R(0), I(OUT2)}), Status::Ok);
    EXPECT_EQ(call("GdipGetCustomLineCapType", {I(cap), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 0);
    EXPECT_EQ(call("GdipGetCustomLineCapBaseInset", {I(cap), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_real(OUT3), 1.0f);
    EXPECT_EQ(call("GdipGetCustomLineCapWidthScale", {I(cap), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_real(OUT3), 1.0f);
    EXPECT_EQ(call("GdipSetCustomLineCapStrokeCaps", {I(cap), I(2), I(3)}), Status::Ok);
    for (u32 bad : {0x11u, 0xFFu, 4u}) {
        EXPECT_EQ(call("GdipSetCustomLineCapStrokeCaps", {I(cap), I(bad), I(0)}),
                  Status::InvalidParameter);
        EXPECT_EQ(call("GdipSetCustomLineCapBaseCap", {I(cap), I(bad)}), Status::InvalidParameter);
    }
    EXPECT_EQ(call("GdipGetCustomLineCapStrokeCaps", {I(cap), I(OUT3), I(OUT3 + 4)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 2);
    EXPECT_EQ(read_int(OUT3 + 4), 3);
    EXPECT_EQ(call("GdipSetCustomLineCapStrokeJoin", {I(cap), I(4)}), Status::Ok);
    EXPECT_EQ(call("GdipSetCustomLineCapBaseCap", {I(cap), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipSetCustomLineCapBaseInset", {I(cap), R(-2)}), Status::Ok);
    EXPECT_EQ(call("GdipSetCustomLineCapWidthScale", {I(cap), R(-1)}), Status::Ok);
    EXPECT_EQ(call("GdipGetCustomLineCapWidthScale", {I(cap), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_real(OUT3), -1.0f);
    poison(OUT3);
    EXPECT_EQ(call("GdipCloneCustomLineCap", {I(cap), I(OUT3)}), Status::Ok);
    EXPECT_NE(read_handle(OUT3), cap);
    EXPECT_EQ(call("GdipGetAdjustableArrowCapHeight", {I(cap), I(OUT3)}), Status::InvalidParameter);

    // On a pen: set stores a copy and LineCapCustom; get clones; a plain cap clears it.
    const u64 p = pen(0xFF000000, 2);
    poison(OUT3);
    EXPECT_EQ(call("GdipGetPenCustomStartCap", {I(p), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_handle(OUT3), 0u);
    EXPECT_EQ(call("GdipSetPenCustomStartCap", {I(p), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetPenCustomStartCap", {I(p), I(cap)}), Status::Ok);
    EXPECT_EQ(pen_obj(p)->start_cap, LineCapCustom);
    poison(OUT3);
    EXPECT_EQ(call("GdipGetPenCustomStartCap", {I(p), I(OUT3)}), Status::Ok);
    const u64 back = read_handle(OUT3);
    EXPECT_NE(back, cap);
    ASSERT_NE(mgr.objects().get<GpCustomLineCap>(back), nullptr);
    EXPECT_EQ(mgr.objects().get<GpCustomLineCap>(back)->base_inset, -2.0f);
    EXPECT_EQ(call("GdipSetPenCustomEndCap", {I(p), I(cap)}), Status::Ok);
    EXPECT_EQ(call("GdipSetPenEndCap", {I(p), I(2)}), Status::Ok);
    poison(OUT3);
    EXPECT_EQ(call("GdipGetPenCustomEndCap", {I(p), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_handle(OUT3), 0u);
    EXPECT_EQ(call("GdipDeleteCustomLineCap", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteCustomLineCap", {I(cap)}), Status::Ok);
}

TEST_P(Gdip_draw_Test, AdjustableArrowCap) {
    poison(OUT);
    ASSERT_EQ(call("GdipCreateAdjustableArrowCap", {R(3), R(2), I(1), I(OUT)}), Status::Ok);
    const u64 a = read_handle(OUT);
    EXPECT_EQ(call("GdipGetAdjustableArrowCapHeight", {I(a), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 3.0f);
    EXPECT_EQ(call("GdipGetAdjustableArrowCapWidth", {I(a), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 2.0f);
    EXPECT_EQ(call("GdipGetAdjustableArrowCapMiddleInset", {I(a), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 0.0f);
    EXPECT_EQ(call("GdipGetAdjustableArrowCapFillState", {I(a), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
    EXPECT_EQ(call("GdipGetCustomLineCapType", {I(a), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
    EXPECT_EQ(call("GdipGetCustomLineCapBaseCap", {I(a), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(call("GdipGetCustomLineCapBaseInset", {I(a), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 1.5f);
    EXPECT_EQ(call("GdipSetAdjustableArrowCapHeight", {I(a), R(5)}), Status::Ok);
    EXPECT_EQ(call("GdipSetAdjustableArrowCapWidth", {I(a), R(4)}), Status::Ok);
    EXPECT_EQ(call("GdipSetAdjustableArrowCapMiddleInset", {I(a), R(1)}), Status::Ok);
    EXPECT_EQ(call("GdipSetAdjustableArrowCapFillState", {I(a), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipGetCustomLineCapBaseInset", {I(a), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 1.25f);
    EXPECT_EQ(call("GdipSetAdjustableArrowCapWidth", {I(a), R(0)}), Status::Ok);
    EXPECT_EQ(call("GdipGetCustomLineCapBaseInset", {I(a), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_real(OUT2), 0.0f);
    EXPECT_EQ(call("GdipCreateAdjustableArrowCap", {R(-1), R(2), I(1), I(OUT)}), Status::Ok);
    EXPECT_EQ(call("GdipCreateAdjustableArrowCap", {R(1), R(2), I(1), I(0)}),
              Status::InvalidParameter);
    poison(OUT3);
    EXPECT_EQ(call("GdipCloneCustomLineCap", {I(a), I(OUT3)}), Status::Ok);
    EXPECT_EQ(call("GdipGetCustomLineCapType", {I(read_handle(OUT3)), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
}

// ---- Draw / Fill validation ---------------------------------------------------

TEST_P(Gdip_draw_Test, PrimitiveValidation) {
    canvas();
    const u64 p = pen();
    const u64 b = brush(0xFF000000);
    write_reals(BUF, {1, 1, 5, 1, 5, 5, 1, 5, 3, 3, 6, 6, 7, 2, 2, 7});
    const GuestAddr pts = BUF;
    const u64 g = graphics;
    EXPECT_EQ(line(p, 0, 0, 1, 1), Status::Ok);
    EXPECT_EQ(call("GdipDrawLine", {I(0), I(p), R(0), R(0), R(1), R(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawLine", {I(g), I(0), R(0), R(0), R(1), R(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawLine", {I(g), I(b), R(0), R(0), R(1), R(1)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipDrawLine", {I(g), I(g), R(0), R(0), R(1), R(1)}), Status::ObjectBusy);
    EXPECT_EQ(line(p, 3, 3, 3, 3), Status::Ok);
    auto draw = [&](const char* name, i32 n) {
        return call(name, {I(g), I(p), I(pts), I(static_cast<u64>(n))});
    };
    for (i32 n : {-1, 0, 1}) EXPECT_EQ(draw("GdipDrawLines", n), Status::InvalidParameter);
    EXPECT_EQ(draw("GdipDrawLines", 2), Status::Ok);
    EXPECT_EQ(call("GdipDrawLines", {I(g), I(p), I(0), I(2)}), Status::InvalidParameter);
    for (i32 n : {1, 2, 3, 4, 7}) EXPECT_EQ(draw("GdipDrawBeziers", n), Status::Ok) << n;
    for (i32 n : {-1, 0, 5, 6, 8}) EXPECT_EQ(draw("GdipDrawBeziers", n), Status::InvalidParameter) << n;
    for (i32 n : {-1, 0, 1}) EXPECT_EQ(draw("GdipDrawCurve", n), Status::InvalidParameter);
    EXPECT_EQ(draw("GdipDrawCurve", 2), Status::Ok);
    EXPECT_EQ(call("GdipDrawCurve2", {I(g), I(p), I(pts), I(4), R(-1)}), Status::Ok);
    EXPECT_EQ(call("GdipDrawCurve2", {I(g), I(p), I(pts), I(1), R(0.5f)}), Status::InvalidParameter);
    for (auto [off, seg, ok] : std::vector<std::tuple<i32, i32, bool>>{
             {0, 3, true}, {0, 4, true}, {1, 3, true}, {3, 1, true}, {2, 2, true}, {4, 0, false},
             {-1, 1, false}, {0, 0, false}, {0, -1, false}, {2, 3, false}, {1, 4, false}}) {
        EXPECT_EQ(call("GdipDrawCurve3", {I(g), I(p), I(pts), I(5), I(static_cast<u64>(off)),
                                          I(static_cast<u64>(seg)), R(0.5f)}),
                  ok ? Status::Ok : Status::InvalidParameter)
            << off << "," << seg;
    }
    for (i32 n : {1, 2}) EXPECT_EQ(draw("GdipDrawClosedCurve", n), Status::InvalidParameter);
    EXPECT_EQ(draw("GdipDrawClosedCurve", 3), Status::Ok);
    EXPECT_EQ(call("GdipDrawClosedCurve2", {I(g), I(p), I(pts), I(4), R(-1)}), Status::Ok);
    for (i32 n : {0, 1}) EXPECT_EQ(draw("GdipDrawPolygon", n), Status::InvalidParameter);
    EXPECT_EQ(draw("GdipDrawPolygon", 2), Status::Ok);
    EXPECT_EQ(call("GdipDrawRectangle", {I(g), I(p), R(1), R(1), R(-1), R(4)}), Status::Ok);
    EXPECT_EQ(call("GdipDrawEllipse", {I(g), I(p), R(1), R(1), R(0), R(4)}), Status::Ok);
    for (const char* name : {"GdipDrawArc", "GdipDrawPie"}) {
        EXPECT_EQ(call(name, {I(g), I(p), R(1), R(1), R(0), R(4), R(0), R(90)}), Status::InvalidParameter);
        EXPECT_EQ(call(name, {I(g), I(p), R(1), R(1), R(4), R(0), R(0), R(90)}), Status::InvalidParameter);
        EXPECT_EQ(call(name, {I(g), I(p), R(1), R(1), R(-1), R(4), R(0), R(90)}), Status::InvalidParameter);
        EXPECT_EQ(call(name, {I(g), I(p), R(1), R(1), R(4), R(4), R(0), R(0)}), Status::Ok);
    }
    EXPECT_EQ(call("GdipFillPie", {I(g), I(b), R(1), R(1), R(0), R(4), R(0), R(90)}), Status::InvalidParameter);
    write_reals(BUF + 0x200, {1, 1, 5, 5, 2, 2, -3, 4});
    for (const char* name : {"GdipDrawRectangles", "GdipFillRectangles"}) {
        const u64 obj = name[4] == 'D' ? p : b;
        EXPECT_EQ(call(name, {I(g), I(obj), I(BUF + 0x200), I(0)}), Status::InvalidParameter);
        EXPECT_EQ(call(name, {I(g), I(obj), I(BUF + 0x200), I(static_cast<u64>(-1))}), Status::InvalidParameter);
        EXPECT_EQ(call(name, {I(g), I(obj), I(BUF + 0x200), I(2)}), Status::Ok);
        EXPECT_EQ(call(name, {I(g), I(obj), I(0), I(1)}), Status::InvalidParameter);
    }
    EXPECT_EQ(call("GdipDrawPath", {I(g), I(p), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawPath", {I(g), I(p), I(mgr.objects().add(GpPath{}))}), Status::Ok);
    EXPECT_EQ(call("GdipFillRectangle", {I(g), I(0), R(0), R(0), R(1), R(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipFillRectangle", {I(g), I(p), R(0), R(0), R(1), R(1)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipFillRectangle", {I(g), I(b), R(0), R(0), R(-1), R(1)}), Status::Ok);
    auto fpoly = [&](i32 n, i32 mode) {
        return call("GdipFillPolygon", {I(g), I(b), I(pts), I(static_cast<u64>(n)), I(static_cast<u64>(mode))});
    };
    EXPECT_EQ(fpoly(0, 0), Status::InvalidParameter);
    EXPECT_EQ(fpoly(-1, 0), Status::InvalidParameter);
    for (i32 n : {1, 2, 3}) EXPECT_EQ(fpoly(n, 0), Status::Ok);
    EXPECT_EQ(fpoly(4, 2), Status::InvalidParameter);
    EXPECT_EQ(fpoly(4, -1), Status::InvalidParameter);
    EXPECT_EQ(call("GdipFillPolygon", {I(g), I(b), I(0), I(3), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipFillPolygon2", {I(g), I(b), I(pts), I(2)}), Status::Ok);
    EXPECT_EQ(call("GdipFillEllipse", {I(g), I(b), R(0), R(0), R(0), R(1)}), Status::Ok);
    EXPECT_EQ(call("GdipFillPath", {I(g), I(b), I(0)}), Status::InvalidParameter);
    for (i32 n : {1, 2, 3, 4})
        EXPECT_EQ(call("GdipFillClosedCurve", {I(g), I(b), I(pts), I(static_cast<u64>(n))}), Status::Ok);
    EXPECT_EQ(call("GdipFillClosedCurve", {I(g), I(b), I(pts), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipFillClosedCurve2", {I(g), I(b), I(pts), I(4), R(0.5f), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipFillClosedCurve2", {I(g), I(b), I(pts), I(4), R(-1), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipGraphicsClear", {I(0), I(0)}), Status::InvalidParameter);
    // Integer variants read GDI+ Point / Rect.
    write_ints(BUF + 0x300, {1, 1, 5, 5});
    EXPECT_EQ(call("GdipDrawLinesI", {I(g), I(p), I(BUF + 0x300), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawLinesI", {I(g), I(p), I(BUF + 0x300), I(2)}), Status::Ok);
}

// ---- Rendered coverage on a 32bppARGB bitmap ---------------------------------

TEST_P(Gdip_draw_Test, FillPixelCentres) {
    canvas();
    const u64 b = brush(0xFF000000);
    // Default pixel offset: pixel i is centred on i; aliased fills cover
    // centres in [10, 30) (probe fillrect-aliased).
    ASSERT_EQ(call("GdipFillRectangle", {I(graphics), I(b), R(10), R(10), R(20), R(20)}), Status::Ok);
    EXPECT_EQ(alpha(9, 15), 0u);
    EXPECT_EQ(alpha(10, 15), 0xFFu);
    EXPECT_EQ(alpha(29, 15), 0xFFu);
    EXPECT_EQ(alpha(30, 15), 0u);
    EXPECT_EQ(alpha(15, 10), 0xFFu);
    EXPECT_EQ(alpha(15, 9), 0u);
    EXPECT_EQ(px(20, 20), 0xFF000000u);
    canvas();  // edges through pixel centres: left/top in, right/bottom out
    ASSERT_EQ(call("GdipFillRectangle", {I(graphics), I(b), R(10.5f), R(10.5f), R(20), R(20)}), Status::Ok);
    EXPECT_EQ(alpha(10, 15), 0u);
    EXPECT_EQ(alpha(11, 15), 0xFFu);
    EXPECT_EQ(alpha(30, 15), 0xFFu);
    EXPECT_EQ(alpha(31, 15), 0u);
    canvas();
    gfx().state.smoothing_mode = 4;  // AntiAlias: edge pixels half covered
    ASSERT_EQ(call("GdipFillRectangle", {I(graphics), I(b), R(10), R(10), R(20), R(20)}), Status::Ok);
    EXPECT_NEAR(static_cast<int>(alpha(10, 15)), 0x80, 3);
    EXPECT_EQ(alpha(11, 15), 0xFFu);
    EXPECT_NEAR(static_cast<int>(alpha(30, 15)), 0x80, 3);
    EXPECT_EQ(alpha(31, 15), 0u);
    canvas();
    gfx().state.smoothing_mode = 4;
    gfx().state.pixel_offset_mode = 4;  // Half: crisp
    ASSERT_EQ(call("GdipFillRectangle", {I(graphics), I(b), R(10), R(10), R(20), R(20)}), Status::Ok);
    EXPECT_EQ(alpha(9, 15), 0u);
    EXPECT_EQ(alpha(10, 15), 0xFFu);
    EXPECT_EQ(alpha(29, 15), 0xFFu);
    EXPECT_EQ(alpha(30, 15), 0u);
    // Ellipse interior/exterior; star fill modes (probe star *).
    canvas();
    ASSERT_EQ(call("GdipFillEllipse", {I(graphics), I(b), R(10), R(10), R(20), R(20)}), Status::Ok);
    EXPECT_EQ(alpha(20, 20), 0xFFu);
    EXPECT_EQ(alpha(12, 12), 0u);
    EXPECT_EQ(alpha(8, 20), 0u);
    write_reals(BUF, {32, 2, 50, 60, 4, 22, 60, 22, 14, 60});
    canvas();
    ASSERT_EQ(call("GdipFillPolygon", {I(graphics), I(b), I(BUF), I(5), I(0)}), Status::Ok);
    EXPECT_EQ(px(32, 32), 0u);
    EXPECT_EQ(px(32, 10), 0xFF000000u);
    canvas();
    ASSERT_EQ(call("GdipFillPolygon", {I(graphics), I(b), I(BUF), I(5), I(1)}), Status::Ok);
    EXPECT_EQ(px(32, 32), 0xFF000000u);
    canvas();
    ASSERT_EQ(call("GdipFillPolygon2", {I(graphics), I(b), I(BUF), I(5)}), Status::Ok);
    EXPECT_EQ(px(32, 32), 0u);
}

TEST_P(Gdip_draw_Test, LineCoverage) {
    canvas();
    const u64 p1 = pen(0xFF000000, 1);
    ASSERT_EQ(line(p1, 10, 20, 50, 20), Status::Ok);
    for (int y = 17; y <= 23; ++y) EXPECT_EQ(alpha(30, y), y == 20 ? 0xFFu : 0u) << y;
    // A nominal aliased line covers both end pixels (probe line-w1-y20-ends).
    EXPECT_EQ(alpha(9, 20), 0u);
    EXPECT_EQ(alpha(10, 20), 0xFFu);
    EXPECT_EQ(alpha(50, 20), 0xFFu);
    EXPECT_EQ(alpha(51, 20), 0u);
    canvas();
    gfx().state.smoothing_mode = 4;
    ASSERT_EQ(line(p1, 10, 20.5f, 50, 20.5f), Status::Ok);
    EXPECT_NEAR(static_cast<int>(alpha(30, 20)), 0x80, 3);
    EXPECT_NEAR(static_cast<int>(alpha(30, 21)), 0x80, 3);
    EXPECT_EQ(alpha(30, 19), 0u);
    // Thin and zero widths draw one device pixel, even antialiased.
    canvas();
    gfx().state.smoothing_mode = 4;
    ASSERT_EQ(line(pen(0xFF000000, 0.3f), 10, 20, 50, 20), Status::Ok);
    EXPECT_EQ(alpha(30, 20), 0xFFu);
    EXPECT_EQ(alpha(30, 19), 0u);
    canvas();
    gfx().state.world = Affine{4, 0, 0, 4, 0, 0};
    ASSERT_EQ(line(pen(0xFF000000, 0), 2, 5, 12, 5), Status::Ok);
    for (int y = 18; y <= 22; ++y) EXPECT_EQ(alpha(30, y), y == 20 ? 0xFFu : 0u) << y;
    // UnitWorld scales with the world transform; UnitPixel does not.
    canvas();
    gfx().state.world = Affine{4, 0, 0, 4, 0, 0};
    ASSERT_EQ(line(p1, 2, 5, 12, 5), Status::Ok);
    for (int y = 16; y <= 23; ++y) EXPECT_EQ(alpha(30, y), (y >= 18 && y <= 21) ? 0xFFu : 0u) << y;
    canvas();
    gfx().state.world = Affine{4, 0, 0, 4, 0, 0};
    ASSERT_EQ(line(pen(0xFF000000, 2, UnitPixel), 2, 5, 12, 5), Status::Ok);
    for (int y = 17; y <= 22; ++y) EXPECT_EQ(alpha(30, y), (y == 19 || y == 20) ? 0xFFu : 0u) << y;
    // Physical pen units draw nothing (probe r2 unit3..6).
    canvas();
    ASSERT_EQ(line(pen(0xFF000000, 3, UnitPoint), 4, 32, 60, 32), Status::Ok);
    EXPECT_EQ(count_px(), 0);
    canvas();
    const u64 p4 = pen(0xFF000000, 4, UnitWorld);
    ASSERT_EQ(line(p4, 4, 32, 60, 32), Status::Ok);
    EXPECT_EQ(count_px(), 224);
    // Width 6, round start, arrow-anchor end (probe line-w6-*).
    canvas();
    const u64 p6 = pen(0xFF000000, 6);
    ASSERT_EQ(line(p6, 10, 20, 50, 20), Status::Ok);
    for (int y = 14; y <= 25; ++y) EXPECT_EQ(alpha(30, y), (y >= 17 && y <= 22) ? 0xFFu : 0u) << y;
    canvas();
    ASSERT_EQ(call("GdipSetPenStartCap", {I(p6), I(LineCapRound)}), Status::Ok);
    ASSERT_EQ(call("GdipSetPenEndCap", {I(p6), I(LineCapArrowAnchor)}), Status::Ok);
    ASSERT_EQ(line(p6, 10, 20, 40, 20), Status::Ok);
    EXPECT_EQ(alpha(6, 20), 0u);
    EXPECT_EQ(alpha(8, 20), 0xFFu);
    EXPECT_EQ(alpha(39, 20), 0xFFu);
    EXPECT_EQ(alpha(41, 20), 0u);
    EXPECT_EQ(alpha(38, 20), 0xFFu);
    EXPECT_EQ(alpha(38, 17), 0u);
    EXPECT_EQ(alpha(32, 24), 0xFFu);  // inside the arrow head, outside the shaft
}

TEST_P(Gdip_draw_Test, ShapeOutlines) {
    canvas();
    const u64 p1 = pen(0xFF000000, 1);
    ASSERT_EQ(call("GdipDrawRectangle", {I(graphics), I(p1), R(10), R(10), R(20), R(20)}), Status::Ok);
    EXPECT_EQ(alpha(10, 20), 0xFFu);
    EXPECT_EQ(alpha(30, 20), 0xFFu);
    EXPECT_EQ(alpha(20, 10), 0xFFu);
    EXPECT_EQ(alpha(20, 30), 0xFFu);
    EXPECT_EQ(alpha(11, 20), 0u);
    EXPECT_EQ(alpha(9, 20), 0u);
    EXPECT_EQ(alpha(20, 20), 0u);
    canvas();
    const u64 p6 = pen(0xFF000000, 6);
    ASSERT_EQ(call("GdipDrawRectangle", {I(graphics), I(p6), R(10), R(10), R(20), R(20)}), Status::Ok);
    for (int x = 4; x <= 16; ++x) EXPECT_EQ(alpha(x, 20), (x >= 7 && x <= 12) ? 0xFFu : 0u) << x;
    canvas();
    ASSERT_EQ(call("GdipSetPenMode", {I(p6), I(PenAlignmentInset)}), Status::Ok);
    ASSERT_EQ(call("GdipDrawRectangle", {I(graphics), I(p6), R(10), R(10), R(20), R(20)}), Status::Ok);
    for (int x = 4; x <= 18; ++x) EXPECT_EQ(alpha(x, 20), (x >= 10 && x <= 15) ? 0xFFu : 0u) << x;
    for (int x = 24; x <= 34; ++x) EXPECT_EQ(alpha(x, 20), (x >= 24 && x <= 29) ? 0xFFu : 0u) << x;
    // Dashes: 3-on 1-off in pen widths along the line.
    canvas();
    const u64 pd = pen(0xFF000000, 2);
    ASSERT_EQ(call("GdipSetPenDashStyle", {I(pd), I(DashStyleDash)}), Status::Ok);
    ASSERT_EQ(line(pd, 0, 20, 40, 20), Status::Ok);
    EXPECT_EQ(alpha(3, 20), 0xFFu);
    EXPECT_EQ(alpha(7, 20), 0u);
    EXPECT_EQ(alpha(10, 20), 0xFFu);
    EXPECT_EQ(alpha(15, 20), 0u);
    // A brush-filled pen paints with its brush's colour.
    canvas();
    const u64 pc = pen(0xFF0000FF, 4);
    ASSERT_EQ(line(pc, 10, 40, 50, 40), Status::Ok);
    EXPECT_EQ(px(30, 40), 0xFF0000FFu);
}

TEST_P(Gdip_draw_Test, GraphicsClear) {
    canvas(0xFF0000FF);
    gfx().state.compositing_mode = 0;
    ASSERT_EQ(call("GdipGraphicsClear", {I(graphics), I(0x80FF0000)}), Status::Ok);
    EXPECT_EQ(px(5, 5), 0x80FF0000u);  // replaced, not blended
    EXPECT_EQ(px(63, 63), 0x80FF0000u);
    ASSERT_EQ(call("GdipGraphicsClear", {I(graphics), I(0)}), Status::Ok);
    EXPECT_EQ(px(5, 5), 0u);
    // SourceOver fill blends; SourceCopy replaces (probe fill over / copy).
    canvas(0xFF0000FF);
    const u64 half = brush(0x80FF0000);
    ASSERT_EQ(call("GdipFillRectangle", {I(graphics), I(half), R(0), R(0), R(10), R(10)}), Status::Ok);
    const u32 over = px(5, 5);
    EXPECT_EQ(over >> 24, 0xFFu);
    EXPECT_NEAR(static_cast<int>((over >> 16) & 0xFF), 0x80, 2);
    EXPECT_NEAR(static_cast<int>(over & 0xFF), 0x7F, 2);
    gfx().state.compositing_mode = 1;
    ASSERT_EQ(call("GdipFillRectangle", {I(graphics), I(half), R(0), R(0), R(10), R(10)}), Status::Ok);
    EXPECT_EQ(px(5, 5), 0x80FF0000u);
}

INSTANTIATE_GDIP_BITNESS(Gdip_draw_Test);

}  // namespace
}  // namespace vitreo::winapi::gdip_test
