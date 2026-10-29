// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_pen_brush.cpp
/// @brief GDI+ area: brushes (solid, hatch, texture, linear and path
/// gradients) and the brush fill (see gdiplus_pen_brush.hpp).
///
/// Every value below that Learn does not state comes from the winoracle
/// probe tools/oracle_probe/gdiplus_brush_probe.c; the probe row is named
/// where a rule is not obvious. Contract notes and deviations:
/// .docs/contracts/gdiplus/pen_brush.md.

#include "winapi/graphics/gdiplus/gdiplus_pen_brush.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_codec.hpp"
#include "winapi/graphics/gdiplus/gdiplus_geometry.hpp"
#include "winapi/graphics/gdiplus/gdiplus_pen.hpp"
#include "winapi/graphics/gdiplus/gdiplus_render.hpp"
#include "winapi/core/marshaler.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vitreo::winapi::gdip {

namespace {

constexpr i32 kHatchStyleMax = 52;  // GdiPlusEnums.h HatchStyleMax
constexpr i32 kTableSteps = 16;     // gradient colour table: 17 entries at i/16

// ---- Small math ------------------------------------------------------------

/// GDI+ REAL -> INT (GpRound): floor(x + 0.5). Probe: "pg centerI round
/// 2.5,-2.5 -> 3 -2".
i32 round_real_gp(float v) { return static_cast<i32>(std::floor(v + 0.5f)); }

bool invert(const Affine& a, Affine& out) {
    const double det = static_cast<double>(a.m11) * a.m22 - static_cast<double>(a.m12) * a.m21;
    if (det == 0.0 || !std::isfinite(det)) return false;
    const double i11 = a.m22 / det, i12 = -a.m12 / det, i21 = -a.m21 / det, i22 = a.m11 / det;
    out.m11 = static_cast<float>(i11);
    out.m12 = static_cast<float>(i12);
    out.m21 = static_cast<float>(i21);
    out.m22 = static_cast<float>(i22);
    out.dx = static_cast<float>(-(a.dx * i11 + a.dy * i21));
    out.dy = static_cast<float>(-(a.dx * i12 + a.dy * i22));
    return true;
}

bool invertible(const Affine& a) {
    Affine unused;
    return invert(a, unused);
}

Affine rotation(float degrees) {
    const double r = static_cast<double>(degrees) * 3.14159265358979323846 / 180.0;
    const float c = static_cast<float>(std::cos(r)), s = static_cast<float>(std::sin(r));
    return {c, s, -s, c, 0, 0};
}

/// The brush transform GDI+ derives for a gradient across @p rect at
/// @p angle degrees (probe "angle * scal *" rows). Scalable: the angle is
/// taken in the unit square and stretched with the rect; otherwise it is
/// the true world angle. Either way the rect centre stays fixed and the
/// gradient span covers the rect's projection on the direction.
Affine angle_transform(const RectF& r, float angle, bool scalable) {
    const double rad = static_cast<double>(angle) * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);
    const double w = r.Width, h = r.Height;
    double m11, m12, m21, m22;
    if (scalable) {
        const double k = std::fabs(c) + std::fabs(s);
        m11 = k * c;
        m12 = k * s * h / w;
        m21 = -k * s * w / h;
        m22 = k * c;
    } else {
        const double k1 = (w * std::fabs(c) + h * std::fabs(s)) / w;
        const double k2 = (w * std::fabs(s) + h * std::fabs(c)) / h;
        m11 = k1 * c;
        m12 = k1 * s;
        m21 = -k2 * s;
        m22 = k2 * c;
    }
    const double cx = r.X + w / 2, cy = r.Y + h / 2;
    return {static_cast<float>(m11), static_cast<float>(m12), static_cast<float>(m21),
            static_cast<float>(m22), static_cast<float>(cx - (cx * m11 + cy * m21)),
            static_cast<float>(cy - (cx * m12 + cy * m22))};
}

// ---- Hatch cells ----------------------------------------------------------------

/// Foreground weight (0 = background .. 255 = foreground) of pixel (x, y)
/// of HatchStyle @p style's 8x8 cell. These cells are Vitreo's own designs,
/// drawn from the textual descriptions on the Learn HatchStyle enumeration
/// page (line direction, relative spacing and width, percentages, and the
/// named motifs); they are not Windows' bitmaps. What the probe establishes
/// is only the behaviour around them: period 8, device-pixel anchoring
/// shifted by the rendering origin, world transform ignored.
u8 hatch_weight(i32 style, i32 x, i32 y) {
    // 8x8 ordered-dither thresholds (Bayer): a p-percent hatch sets the
    // round(p * 64) pixels with the lowest thresholds ("ratio of foreground
    // to background is p:100").
    static constexpr u8 kBayer[8][8] = {
        {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
        {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
        {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
        {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
    };
    static constexpr i32 kPercent[12] = {5, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90};
    const i32 d = (x - y) & 7;  // downward (right-slanting) diagonals
    const i32 u = (x + y) & 7;  // upward (left-slanting) diagonals
    auto on = [](bool b) -> u8 { return b ? 255 : 0; };
    // "The lines are antialiased": a one-pixel line with its neighbours
    // partly covered.
    auto aa = [](i32 dist) -> u8 { return dist == 0 ? 255 : (dist == 1 || dist == 7) ? 96 : 0; };
    switch (style) {
    case 0: return on(y == 0);                                   // Horizontal
    case 1: return on(x == 0);                                   // Vertical
    case 2: return aa(d);                                        // ForwardDiagonal
    case 3: return aa((u + 1) & 7);                              // BackwardDiagonal
    case 4: return on(x == 0 || y == 0);                         // Cross
    case 5: return std::max(aa(d), aa((u + 1) & 7));             // DiagonalCross
    case 6: case 7: case 8: case 9: case 10: case 11:
    case 12: case 13: case 14: case 15: case 16: case 17: {      // NN Percent
        const i32 count = (kPercent[style - 6] * 64 + 50) / 100;
        return on(kBayer[y][x] < count);
    }
    case 18: return on(((x - y) & 3) == 0);                      // LightDownwardDiagonal: half the spacing
    case 19: return on(((x + y) & 3) == 3);                      // LightUpwardDiagonal
    case 20: return on(((x - y) & 3) < 2);                       // DarkDownwardDiagonal: also twice the width
    case 21: return on(((x + y) & 3) >= 2);                      // DarkUpwardDiagonal
    case 22: return on(d < 3);                                   // WideDownwardDiagonal: triple width
    case 23: return on(u >= 5);                                  // WideUpwardDiagonal
    case 24: return on((x & 3) == 0);                            // LightVertical
    case 25: return on((y & 3) == 0);                            // LightHorizontal
    case 26: return on((x & 1) == 0);                            // NarrowVertical: a quarter of the spacing
    case 27: return on((y & 1) == 0);                            // NarrowHorizontal
    case 28: return on((x & 3) < 2);                             // DarkVertical
    case 29: return on((y & 3) < 2);                             // DarkHorizontal
    case 30: return on(y < 4 && ((x - y) & 3) == 0);             // DashedDownwardDiagonal: rows of "\" dashes
    case 31: return on(y < 4 && ((x + y) & 3) == 3);             // DashedUpwardDiagonal: rows of "/" dashes
    case 32: return on((y == 0 && x < 4) || (y == 4 && x >= 4)); // DashedHorizontal
    case 33: return on((x == 0 && y < 4) || (x == 4 && y >= 4)); // DashedVertical
    case 34: return on(x == ((y * 3 + 1) & 7));                  // SmallConfetti: scattered dots
    case 35: {                                                   // LargeConfetti: scattered 2x2 pieces
        static constexpr i32 kPieces[3][2] = {{0, 0}, {5, 2}, {2, 5}};
        for (const auto& p : kPieces)
            if (((x - p[0]) & 7) < 2 && ((y - p[1]) & 7) < 2) return 255;
        return 0;
    }
    case 36: {                                                   // ZigZag
        static constexpr i32 kHeight[4] = {0, 1, 2, 1};
        return on((y & 3) == kHeight[x & 3]);
    }
    case 37: {                                                   // Wave: a row of tildes
        static constexpr i32 kHeight[8] = {1, 0, 0, 1, 2, 3, 3, 2};
        return on(y == kHeight[x]);
    }
    case 38: return on(u == 7 || (d == 4 && u < 4));             // DiagonalBrick: courses along the backward diagonal
    case 39: return on(y == 0 || y == 4 || (x == 0 && y < 4) || (x == 4 && y > 4));  // HorizontalBrick
    case 40: {                                                   // Weave: strands alternating over and under
        const bool block = (((x >> 2) + (y >> 2)) & 1) != 0;
        return on(block ? ((x + y) & 3) == 3 : ((x - y) & 3) == 0);
    }
    case 41: return on(x < 2 || (y < 3 && ((x + y) & 1) == 0));  // Plaid: half-toned bands over solid stripes
    case 42: return on((y == 1 && x == 1) || (y == 2 && x == 2) || (y == 3 && x == 1) ||   // Divot: ">" ...
                       (y == 5 && x == 6) || (y == 6 && x == 5) || (y == 7 && x == 6));    // ... and "<"
    case 43: return on((y == 0 && (x & 1) == 0) || (x == 0 && (y & 1) == 0));  // DottedGrid
    case 44: return on((d == 0 || u == 0) && (x & 1) == 0);     // DottedDiamond
    case 45: return on(d == 0 || (y == 0 && x >= 4));            // Shingle: forward-diagonal shingles with a ledge
    case 46: return on(d < 2 || u < 2);                          // Trellis: wide crossing slats
    case 47: {                                                   // Sphere: a checkerboard of shaded balls
        static constexpr u8 kBall[4] = {0x6, 0xB, 0xF, 0x6};     // rows of a 4x4 ball, highlight top-left
        if ((((x >> 2) + (y >> 2)) & 1) != 0) return 0;
        return on((kBall[y & 3] >> (3 - (x & 3))) & 1);
    }
    case 48: return on((x & 3) == 0 || (y & 3) == 0);            // SmallGrid: half the Cross spacing
    case 49: return on((((x >> 1) + (y >> 1)) & 1) == 0);        // SmallCheckerBoard
    case 50: return on((((x >> 2) + (y >> 2)) & 1) == 0);        // LargeCheckerBoard: twice the squares
    case 51: return on(d == 0 || u == 7);                        // OutlinedDiamond: the cross without antialiasing
    case 52: return on(std::abs(2 * x - 7) + std::abs(2 * y - 7) <= 8);  // SolidDiamond
    default: return 0;
    }
}

struct HatchCells {
    u8 w[kHatchStyleMax + 1][64];
    HatchCells() {
        for (i32 s = 0; s <= kHatchStyleMax; ++s)
            for (i32 i = 0; i < 64; ++i) w[s][i] = hatch_weight(s, i % 8, i / 8);
    }
};

const HatchCells& hatch_cells() {
    static const HatchCells cells;
    return cells;
}

// ---- Colour ------------------------------------------------------------------

/// Premultiplied colour, channels 0..255.
struct PColor {
    float a{0}, r{0}, g{0}, b{0};
};

PColor premul(u32 argb) {
    const float a = static_cast<float>(argb >> 24);
    const float k = a / 255.0f;
    return {a, static_cast<float>((argb >> 16) & 0xFF) * k, static_cast<float>((argb >> 8) & 0xFF) * k,
            static_cast<float>(argb & 0xFF) * k};
}

PColor lerp(const PColor& x, const PColor& y, float f) {
    return {x.a + (y.a - x.a) * f, x.r + (y.r - x.r) * f, x.g + (y.g - x.g) * f,
            x.b + (y.b - x.b) * f};
}

/// Mix two ARGB colours at @p f, in gamma-2.2 linear light when @p gamma
/// (GDI+ gamma correction; probe "linefill gamma" matches 2.2 exactly).
PColor mix(u32 c1, u32 c2, float f, bool gamma) {
    if (!gamma) return lerp(premul(c1), premul(c2), f);
    auto lin = [](u32 c, int shift) { return std::pow(((c >> shift) & 0xFF) / 255.0, 2.2); };
    auto chan = [&](int shift) {
        const double v = lin(c1, shift) + (lin(c2, shift) - lin(c1, shift)) * f;
        return static_cast<float>(255.0 * std::pow(std::clamp(v, 0.0, 1.0), 1.0 / 2.2));
    };
    const float a = static_cast<float>(c1 >> 24) + (static_cast<float>(c2 >> 24) - static_cast<float>(c1 >> 24)) * f;
    const float k = std::clamp(a, 0.0f, 255.0f) / 255.0f;
    return {a, chan(16) * k, chan(8) * k, chan(0) * k};
}

/// Store as premultiplied B,G,R,A bytes (the render canvas format).
void store(const PColor& c, u8* px) {
    const float a = std::clamp(c.a, 0.0f, 255.0f);
    auto q = [&](float v) { return static_cast<u8>(std::lround(std::clamp(v, 0.0f, a))); };
    px[0] = q(c.b);
    px[1] = q(c.g);
    px[2] = q(c.r);
    px[3] = static_cast<u8>(std::lround(a));
}

PColor load(const u8* px) {
    return {static_cast<float>(px[3]), static_cast<float>(px[2]), static_cast<float>(px[1]),
            static_cast<float>(px[0])};
}

// ---- Blend curves ----------------------------------------------------------

/// Piecewise-linear lookup of @p values over @p positions at @p t.
/// Positions need not be monotonic (GDI+ accepts them); the first segment
/// that brackets t wins.
template <typename F>
void piecewise(const std::vector<float>& positions, float t, F&& at) {
    const usize n = positions.size();
    for (usize i = 0; i + 1 < n; ++i) {
        const float p0 = positions[i], p1 = positions[i + 1];
        if (t >= std::min(p0, p1) && t <= std::max(p0, p1)) {
            at(i, p1 == p0 ? 0.0f : (t - p0) / (p1 - p0));
            return;
        }
    }
    at(t <= positions.front() ? 0 : n - 2, t <= positions.front() ? 0.0f : 1.0f);
}

/// The blend factor at @p t (0..1 along the gradient, 0 = first colour /
/// path boundary). One entry: t^factor (path gradients run the exponent
/// from the centre: 1 - (1-t)^factor). Probe "linefill/pgfill blend1 .5".
float blend_factor(const GradientBlend& b, float t, bool path) {
    if (b.factors.size() <= 1) {
        const float f = b.factors.empty() ? 1.0f : b.factors[0];
        return path ? 1.0f - std::pow(1.0f - t, f) : std::pow(t, f);
    }
    float out = 0;
    piecewise(b.positions, t, [&](usize i, float u) {
        out = b.factors[i] + (b.factors[i + 1] - b.factors[i]) * u;
    });
    return out;
}

/// One entry of the gradient colour table at @p t. GDI+ evaluates the
/// gradient at t = i/16 (17 entries) and interpolates linearly between them
/// (probe "linefill long preset": knots smoothed identically at 10 px and
/// 1000 px; "linefill gamma" matches only through the table).
PColor entry(const GpBrush& b, float t, u32 from, u32 to, bool path) {
    const bool gamma = b.gamma != 0;
    if (!b.preset.colors.empty()) {
        PColor out;
        piecewise(b.preset.positions, t, [&](usize i, float u) {
            out = mix(b.preset.colors[i], b.preset.colors[i + 1], u, gamma);
        });
        return out;
    }
    return mix(from, to, blend_factor(b.blend, t, path), gamma);
}

struct ColorTable {
    PColor e[kTableSteps + 1];
    [[nodiscard]] PColor at(float t) const {
        const float x = std::clamp(t, 0.0f, 1.0f) * kTableSteps;
        const i32 i = std::min(static_cast<i32>(x), kTableSteps - 1);
        return lerp(e[i], e[i + 1], x - static_cast<float>(i));
    }
};

// ---- Brush sampling ----------------------------------------------------------

/// Wrap a texel index per WrapMode on one axis (@p flip: this axis mirrors).
/// Returns -1 for Clamp outside the image.
i32 wrap_index(i32 i, i32 n, i32 wrap, bool flip) {
    if (wrap == WrapModeClamp) return (i < 0 || i >= n) ? -1 : i;
    if (flip) {
        i32 m = i % (2 * n);
        if (m < 0) m += 2 * n;
        return m >= n ? 2 * n - 1 - m : m;
    }
    i32 m = i % n;
    return m < 0 ? m + n : m;
}

/// Bilinear texel sample at brush-space (u, v); texel i is centred on i.
/// GDI+ filters texture brushes bilinearly whatever the graphics
/// InterpolationMode (probe "texfill nearest scale2").
PColor sample_texture(const u8* bgra, i32 w, i32 h, i32 wrap, double u, double v) {
    const bool fx = wrap == WrapModeTileFlipX || wrap == WrapModeTileFlipXY;
    const bool fy = wrap == WrapModeTileFlipY || wrap == WrapModeTileFlipXY;
    const double iu = std::floor(u), iv = std::floor(v);
    const float tu = static_cast<float>(u - iu), tv = static_cast<float>(v - iv);
    auto texel = [&](double x, double y) -> PColor {
        if (std::fabs(x) > 1e9 || std::fabs(y) > 1e9) return {};
        const i32 xi = wrap_index(static_cast<i32>(x), w, wrap, fx);
        const i32 yi = wrap_index(static_cast<i32>(y), h, wrap, fy);
        if (xi < 0 || yi < 0) return {};
        return load(bgra + (static_cast<usize>(yi) * static_cast<usize>(w) + static_cast<usize>(xi)) * 4u);
    };
    const PColor top = lerp(texel(iu, iv), texel(iu + 1, iv), tu);
    const PColor bot = lerp(texel(iu, iv + 1), texel(iu + 1, iv + 1), tu);
    return lerp(top, bot, tv);
}

/// A path gradient's boundary in brush space: every point of the path as
/// ONE polygon (GDI+ joins the figures; probe "pgfill 2rects"), Béziers
/// flattened, with the surround colour of each vertex.
struct PathPolygon {
    std::vector<PointF> v;
    std::vector<PColor> c;
};

PathPolygon build_polygon(const GpBrush& b) {
    PathPolygon poly;
    const auto& pts = b.path_points;
    const auto& types = b.path_types;
    auto color_of = [&](usize i) {
        return premul(i < b.surround.size() ? b.surround[i] : b.surround.back());
    };
    for (usize i = 0; i < pts.size(); ++i) {
        const u8 kind = i < types.size() ? (types[i] & PathPointTypePathTypeMask) : PathPointTypeLine;
        if (kind == PathPointTypeBezier && i >= 1 && i + 2 < pts.size()) {
            const PointF p0 = pts[i - 1], p1 = pts[i], p2 = pts[i + 1], p3 = pts[i + 2];
            const PColor c0 = color_of(i - 1), c3 = color_of(i + 2);
            constexpr int kSteps = 16;
            for (int s = 1; s <= kSteps; ++s) {
                const float t = static_cast<float>(s) / kSteps, mt = 1 - t;
                const float a = mt * mt * mt, bb = 3 * mt * mt * t, cc = 3 * mt * t * t, d = t * t * t;
                poly.v.push_back({a * p0.X + bb * p1.X + cc * p2.X + d * p3.X,
                                  a * p0.Y + bb * p1.Y + cc * p2.Y + d * p3.Y});
                poly.c.push_back(lerp(c0, c3, t));
            }
            i += 2;
            continue;
        }
        poly.v.push_back(pts[i]);
        poly.c.push_back(color_of(i));
    }
    return poly;
}

/// Colour of a path gradient at brush-space @p q, or false outside every
/// triangle of the fan from the centre to the boundary edges. GDI+ paints
/// the fan, not the polygon interior, and where triangles overlap the later
/// edge wins (probe "pgfill concave", "pgfill 2rects"). With focus scales
/// the triangle is chosen by the sector of the scaled (inner) edge and the
/// outer edge is taken as its full line (probe "pgfill focus.5,.25 y=4").
/// Colours are evaluated exactly per pixel: unlike the linear gradient,
/// interpolation-colour knots are not smoothed (probe "pgfill sq preset").
struct PathSampler {
    const GpBrush& b;
    PathPolygon poly;
    PColor center;
    bool focused;

    explicit PathSampler(const GpBrush& brush)
        : b(brush), poly(build_polygon(brush)), center(premul(brush.center_color)),
          focused(brush.focus.X != 0 || brush.focus.Y != 0) {}

    /// Solve d * rho = A + beta (B - A) (all relative to the centre).
    static bool ray_edge(double dx, double dy, double ax, double ay, double bx, double by, double& rho,
                         double& beta) {
        const double ex = bx - ax, ey = by - ay;
        const double det = dx * (-ey) - (-ex) * dy;
        if (std::fabs(det) < 1e-12) return false;
        rho = (ax * (-ey) - (-ex) * ay) / det;
        beta = (dx * ay - dy * ax) / det;
        return true;
    }

    PColor shade(const PColor& edge, double t) const {
        const float tf = static_cast<float>(std::clamp(t, 0.0, 1.0));
        if (!b.preset.colors.empty()) return entry(b, tf, 0, 0, true);
        const float f = blend_factor(b.blend, tf, true);
        if (b.gamma == 0) return lerp(edge, center, f);
        auto unp = [](const PColor& c) {
            if (c.a <= 0) return 0u;
            auto ch = [&](float v) {
                return static_cast<u32>(std::lround(std::clamp(v * 255.0f / c.a, 0.0f, 255.0f)));
            };
            return (static_cast<u32>(std::lround(c.a)) << 24) | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
        };
        return mix(unp(edge), b.center_color, f, true);
    }

    /// Whether a point exactly on the outer edge A->B (relative to the
    /// centre) is painted: GDI+ follows the top-left rule (the left and top
    /// edges of a square are painted, its right and bottom edges are not:
    /// probe "pgfill sq clamp" x=20 / y=20).
    static bool owns_edge(double ax, double ay, double bx, double by) {
        double nx = by - ay, ny = -(bx - ax);
        if (nx * ax + ny * ay < 0) {  // point the normal away from the centre
            nx = -nx;
            ny = -ny;
        }
        return nx < 0 || (nx == 0 && ny < 0);
    }

    bool sample(PointF q, PColor& out) const {
        const usize n = poly.v.size();
        if (n < 2) return false;
        const double dx = q.X - b.center.X, dy = q.Y - b.center.Y;
        constexpr double eps = 1e-6;
        // Coverage and (without focus) the colour: the containing fan
        // triangle, the later edge winning.
        for (usize kk = n; kk-- > 0;) {
            const usize k2 = (kk + 1) % n;
            const double ax = poly.v[kk].X - b.center.X, ay = poly.v[kk].Y - b.center.Y;
            const double bx = poly.v[k2].X - b.center.X, by = poly.v[k2].Y - b.center.Y;
            const double det = ax * by - ay * bx;
            if (std::fabs(det) < 1e-12) continue;
            const double wa = (dx * by - dy * bx) / det;
            const double wb = (ax * dy - ay * dx) / det;
            if (wa < -eps || wb < -eps) continue;
            const double r = wa + wb;
            if (r > 1 + eps) continue;
            if (r > 1 - eps) {
                if (!owns_edge(ax, ay, bx, by)) continue;
                // A vertex belongs to the outline only when both its edges
                // own it (the square's top-right and bottom-left corners
                // are not painted: probe "pgfill sq 4colors").
                auto rel = [&](usize i) {
                    return std::pair<double, double>{poly.v[i % n].X - b.center.X, poly.v[i % n].Y - b.center.Y};
                };
                if (wa < eps) {
                    const auto [cx2, cy2] = rel(k2 + 1);
                    if (!owns_edge(bx, by, cx2, cy2)) continue;
                }
                if (wb < eps) {
                    const auto [px2, py2] = rel(kk + n - 1);
                    if (!owns_edge(px2, py2, ax, ay)) continue;
                }
            }
            if (!focused) {
                const float s = r > 0 ? static_cast<float>(wb / r) : 0.0f;
                out = shade(lerp(poly.c[kk], poly.c[k2], s), 1 - r);
                return true;
            }
            // Focus scales: every edge ramps from its outer line (t = 0) to
            // its scaled inner line (t = 1); the point takes the smallest
            // (probe "pgfill focus.5,.25 y=4": the left ramp near the left
            // edge, the top ramp beyond x = 3).
            if (dx == 0 && dy == 0) {
                out = shade(poly.c[kk], 1.0);
                return true;
            }
            double best = 2;
            PColor edge = poly.c[kk];
            for (usize e = 0; e < n; ++e) {
                const usize e2 = (e + 1) % n;
                const double ex = poly.v[e].X - b.center.X, ey = poly.v[e].Y - b.center.Y;
                const double fx = poly.v[e2].X - b.center.X, fy = poly.v[e2].Y - b.center.Y;
                double rho_o = 0, beta_o = 0, rho_i = 0, beta_i = 0;
                if (!ray_edge(dx, dy, ex, ey, fx, fy, rho_o, beta_o) || rho_o <= eps) continue;
                if (!ray_edge(dx, dy, ex * b.focus.X, ey * b.focus.Y, fx * b.focus.X, fy * b.focus.Y, rho_i, beta_i)) {
                    rho_i = 0;
                }
                rho_i = std::clamp(rho_i, 0.0, rho_o);
                const double t = rho_o - rho_i > 1e-12 ? (rho_o - 1) / (rho_o - rho_i) : 1.0;
                if (t < best) {
                    best = t;
                    edge = lerp(poly.c[e], poly.c[e2], static_cast<float>(std::clamp(beta_o, 0.0, 1.0)));
                }
            }
            out = shade(edge, best > 1 ? 1.0 : best);
            return true;
        }
        return false;
    }
};

}  // namespace

bool brush_solid_argb(const GpBrush& brush, u32& argb) {
    if (brush.type != BrushTypeSolidColor) return false;
    argb = brush.color;
    return true;
}

namespace {

/// Clip @p cg to the pixels @p path covers. Antialiased rendering clips to
/// the path itself. Aliased rendering follows GDI+'s rule: a pixel is
/// painted when its centre is inside, a centre on a left/top edge counts as
/// inside and one on a right/bottom edge as outside (measured by the draw
/// area). Pixels the antialiased coverage marks fully in or out are taken
/// from it; partly covered ones are decided by a point 1/256 px below-right
/// of the centre. Returns false when nothing is covered.
bool clip_to_coverage(RenderScope& scope, CGContextRef cg, CGPathRef path, bool even_odd, Status& st) {
    st = Status::Ok;
    const i32 sm = scope.graphics().state.smoothing_mode;
    if (sm == 2 || sm == 4 || sm == 5) {
        CGContextAddPath(cg, path);
        if (even_odd) CGContextEOClip(cg); else CGContextClip(cg);
        return true;
    }
    const i32 w = scope.width(), h = scope.height();
    const CGAffineTransform ctm = CGContextGetCTM(cg);
    std::vector<u8> mask(static_cast<usize>(w) * static_cast<usize>(h), 0);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef mc = CGBitmapContextCreate(mask.data(), static_cast<size_t>(w), static_cast<size_t>(h), 8,
                                            static_cast<size_t>(w), gray, static_cast<u32>(kCGImageAlphaNone));
    if (mc == nullptr) {
        CGColorSpaceRelease(gray);
        st = Status::OutOfMemory;
        return false;
    }
    CGContextConcatCTM(mc, ctm);
    CGContextSetShouldAntialias(mc, true);
    CGContextSetGrayFillColor(mc, 1.0, 1.0);
    CGContextAddPath(mc, path);
    if (even_odd) CGContextEOFillPath(mc); else CGContextFillPath(mc);
    CGContextFlush(mc);
    CGContextRelease(mc);
    const CGAffineTransform inv = CGAffineTransformInvert(ctm);
    constexpr double kNudge = 1.0 / 256.0;
    bool any = false;
    for (i32 row = 0; row < h; ++row) {
        for (i32 x = 0; x < w; ++x) {
            u8& v = mask[static_cast<usize>(row) * static_cast<usize>(w) + static_cast<usize>(x)];
            if (v != 0 && v != 255) {
                const CGPoint p = CGPointApplyAffineTransform(
                    CGPointMake(x + 0.5 + kNudge, (h - row) - 0.5 - kNudge), inv);
                v = CGPathContainsPoint(path, nullptr, p, even_odd) ? 255 : 0;
            }
            any |= v != 0;
        }
    }
    if (!any) {
        CGColorSpaceRelease(gray);
        return false;
    }
    // The clip keeps referencing the mask after this returns: hand it a copy.
    CFDataRef data = CFDataCreate(nullptr, mask.data(), static_cast<CFIndex>(mask.size()));
    CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    CGImageRef img = CGImageCreate(static_cast<size_t>(w), static_cast<size_t>(h), 8, 8, static_cast<size_t>(w),
                                   gray, static_cast<u32>(kCGImageAlphaNone), dp, nullptr, false,
                                   kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(gray);
    if (img == nullptr) {
        st = Status::OutOfMemory;
        return false;
    }
    CGContextConcatCTM(cg, inv);
    CGContextClipToMask(cg, CGRectMake(0, 0, w, h), img);
    CGContextConcatCTM(cg, ctm);
    CGImageRelease(img);
    return true;
}

}  // namespace

Status fill_path(RenderScope& scope, CGPathRef path, bool even_odd, const GpBrush& brush) {
    CGContextRef cg = scope.cg();
    if (cg == nullptr || path == nullptr) return scope.status();
    u32 argb = 0;
    if (brush_solid_argb(brush, argb)) {
        CGContextSaveGState(cg);
        Status st = Status::Ok;
        // Antialiased: the fill below carries the edge coverage; clipping to
        // the same path first would apply it twice (0.5 coverage -> 0.25).
        const i32 sm = scope.graphics().state.smoothing_mode;
        const bool aa = sm == 2 || sm == 4 || sm == 5;
        if (aa || clip_to_coverage(scope, cg, path, even_odd, st)) {
            CGContextSetRGBFillColor(cg, ((argb >> 16) & 0xFF) / 255.0, ((argb >> 8) & 0xFF) / 255.0,
                                     (argb & 0xFF) / 255.0, ((argb >> 24) & 0xFF) / 255.0);
            CGContextAddPath(cg, path);
            if (even_odd) CGContextEOFillPath(cg); else CGContextFillPath(cg);
        }
        CGContextRestoreGState(cg);
        return st;
    }
    if (brush.type < BrushTypeHatchFill || brush.type > BrushTypeLinearGradient) {
        return Status::InvalidParameter;
    }

    // Every other brush: compute the brush colour of each device pixel the
    // path can touch, then draw that block through the path as the clip.
    const i32 cw = scope.width(), ch = scope.height();
    const CGAffineTransform ctm = CGContextGetCTM(cg);  // world -> CG native device (y up)
    const CGRect dev = CGRectApplyAffineTransform(CGPathGetBoundingBox(path), ctm);
    if (CGRectIsNull(dev) || CGRectIsEmpty(dev) || !std::isfinite(dev.origin.x)) return Status::Ok;
    const i32 x0 = std::max(0, static_cast<i32>(std::floor(dev.origin.x)) - 1);
    const i32 x1 = std::min(cw, static_cast<i32>(std::ceil(dev.origin.x + dev.size.width)) + 1);
    // Memory rows run top-down: native y = ch - row.
    const i32 y0 = std::max(0, ch - static_cast<i32>(std::ceil(dev.origin.y + dev.size.height)) - 1);
    const i32 y1 = std::min(ch, ch - static_cast<i32>(std::floor(dev.origin.y)) + 1);
    if (x0 >= x1 || y0 >= y1) return Status::Ok;
    const i32 bw = x1 - x0, bh = y1 - y0;
    std::vector<u8> block(static_cast<usize>(bw) * static_cast<usize>(bh) * 4u, 0);

    const CGAffineTransform inv_ctm = CGAffineTransformInvert(ctm);
    Affine brush_inv;
    const bool brush_ok = invert(brush.transform, brush_inv);
    const GraphicsState& gs = scope.graphics().state;
    // Device pixel (x, y) -> brush space. GDI+ samples device pixel i at
    // device coordinate i (the graphics area's world_to_device carries the
    // PixelOffsetMode shift, so gradients move by half a pixel under Half:
    // probe "linefill pixoffhalf"); texture-like brushes sample the same
    // texel whatever the mode (probe "texfill pixoffhalf"), i.e. without
    // that shift. This mapping is independent of how the canvas CTM places
    // geometry.
    const bool half = gs.pixel_offset_mode == 2 || gs.pixel_offset_mode == 4;
    Affine dev_inv;
    if (!invert(world_to_device(scope.graphics()), dev_inv)) return Status::Ok;
    auto to_brush = [&](double dx, double dy) {
        return brush_inv.apply(dev_inv.apply({static_cast<float>(dx), static_cast<float>(dy)}));
    };
    const double tex_off = half ? -0.5 : 0.0;

    if (brush.type == BrushTypeHatchFill) {
        const i32 style = std::clamp(brush.hatch_style, 0, kHatchStyleMax);
        const PColor fore = premul(brush.color), back = premul(brush.hatch_back);
        u8 f8[4], b8[4];
        store(fore, f8);
        store(back, b8);
        for (i32 y = y0; y < y1; ++y) {
            for (i32 x = x0; x < x1; ++x) {
                // Anchored to device pixels plus the rendering origin (probe
                // "hatchanchor").
                const i32 hx = (x - gs.rendering_origin_x) & 7, hy = (y - gs.rendering_origin_y) & 7;
                const i32 wgt = hatch_cells().w[style][hy * 8 + hx];
                u8* px = &block[(static_cast<usize>(y - y0) * static_cast<usize>(bw) + static_cast<usize>(x - x0)) * 4u];
                for (int c = 0; c < 4; ++c) {
                    if (wgt == 255) px[c] = f8[c];
                    else if (wgt == 0) px[c] = b8[c];
                    // Partial (antialiased) weights mix premultiplied.
                    else px[c] = static_cast<u8>(b8[c] + ((static_cast<i32>(f8[c]) - b8[c]) * wgt >> 8));
                }
            }
        }
    } else if (!brush_ok) {
        return Status::Ok;  // a singular brush transform paints nothing
    } else if (brush.type == BrushTypeLinearGradient) {
        ColorTable table;
        for (i32 i = 0; i <= kTableSteps; ++i) {
            table.e[i] = entry(brush, static_cast<float>(i) / kTableSteps, brush.line_colors[0],
                               brush.line_colors[1], false);
        }
        const bool flip = brush.wrap == WrapModeTileFlipX || brush.wrap == WrapModeTileFlipXY;
        for (i32 y = y0; y < y1; ++y) {
            for (i32 x = x0; x < x1; ++x) {
                const PointF q = to_brush(x, y);
                double t = (static_cast<double>(q.X) - brush.rect.X) / brush.rect.Width;
                if (!std::isfinite(t)) continue;
                // A t a rounding error away from a tile boundary is on it
                // (probe "linefill angle30s": the corner pixel is colour 1).
                if (std::fabs(t - std::round(t)) < 1e-5) t = std::round(t);
                if (flip) {
                    t = std::fmod(t, 2.0);
                    if (t < 0) t += 2.0;
                    if (t > 1.0) t = 2.0 - t;
                } else {
                    t -= std::floor(t);
                }
                store(table.at(static_cast<float>(t)),
                      &block[(static_cast<usize>(y - y0) * static_cast<usize>(bw) + static_cast<usize>(x - x0)) * 4u]);
            }
        }
    } else if (brush.type == BrushTypeTextureFill) {
        if (!brush.texture || brush.texture->width <= 0 || brush.texture->height <= 0) return Status::Ok;
        const GpImage& t = *brush.texture;
        for (i32 y = y0; y < y1; ++y) {
            for (i32 x = x0; x < x1; ++x) {
                const PointF q = to_brush(x + tex_off, y + tex_off);
                store(sample_texture(t.bits.data(), t.width, t.height, brush.wrap, q.X, q.Y),
                      &block[(static_cast<usize>(y - y0) * static_cast<usize>(bw) + static_cast<usize>(x - x0)) * 4u]);
            }
        }
    } else {  // PathGradient
        const PathSampler sampler(brush);
        if (brush.wrap == WrapModeClamp) {
            for (i32 y = y0; y < y1; ++y) {
                for (i32 x = x0; x < x1; ++x) {
                    PColor c;
                    if (sampler.sample(to_brush(x, y), c)) {
                        store(c, &block[(static_cast<usize>(y - y0) * static_cast<usize>(bw) + static_cast<usize>(x - x0)) * 4u]);
                    }
                }
            }
        } else {
            // Tiled path gradients repeat the gradient over the brush rect
            // as a texture (flips mirror texel indices: probe "pgfill sq flipx").
            const i32 tw = std::max(1, static_cast<i32>(std::ceil(brush.rect.Width)));
            const i32 th = std::max(1, static_cast<i32>(std::ceil(brush.rect.Height)));
            if (static_cast<i64>(tw) * th > (1 << 24)) {
                log_unimplemented("fill_path", "tiled path gradient larger than 16M texels");
                return Status::NotImplemented;
            }
            std::vector<u8> tile(static_cast<usize>(tw) * static_cast<usize>(th) * 4u, 0);
            for (i32 j = 0; j < th; ++j) {
                for (i32 i = 0; i < tw; ++i) {
                    PColor c;
                    if (sampler.sample({brush.rect.X + static_cast<float>(i), brush.rect.Y + static_cast<float>(j)}, c)) {
                        store(c, &tile[(static_cast<usize>(j) * static_cast<usize>(tw) + static_cast<usize>(i)) * 4u]);
                    }
                }
            }
            for (i32 y = y0; y < y1; ++y) {
                for (i32 x = x0; x < x1; ++x) {
                    const PointF q = to_brush(x + tex_off, y + tex_off);
                    store(sample_texture(tile.data(), tw, th, brush.wrap, q.X - brush.rect.X, q.Y - brush.rect.Y),
                          &block[(static_cast<usize>(y - y0) * static_cast<usize>(bw) + static_cast<usize>(x - x0)) * 4u]);
                }
            }
        }
    }

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, block.data(), block.size(), nullptr);
    CGImageRef image = CGImageCreate(static_cast<size_t>(bw), static_cast<size_t>(bh), 8, 32,
                                     static_cast<size_t>(bw) * 4u, space,
                                     static_cast<u32>(kCGImageAlphaPremultipliedFirst) |
                                         static_cast<u32>(kCGBitmapByteOrder32Little),
                                     provider, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    if (image == nullptr) return Status::OutOfMemory;
    CGContextSaveGState(cg);
    Status st = Status::Ok;
    if (clip_to_coverage(scope, cg, path, even_odd, st)) {
        CGContextConcatCTM(cg, inv_ctm);  // user space = native device pixels
        CGContextSetInterpolationQuality(cg, kCGInterpolationNone);
        CGContextDrawImage(cg, CGRectMake(x0, ch - y1, bw, bh), image);
    }
    CGContextRestoreGState(cg);
    CGImageRelease(image);
    return st;
}

namespace {

// ---- Export plumbing ---------------------------------------------------------

using Body = std::function<Status(Marshaler&)>;

void def(HandlerMap& map, const char* name, Body body) {
    map[name] = [body = std::move(body)](Marshaler& m) { write_status(m, body(m)); };
}

bool w32(Marshaler& m, GuestAddr at, u32 v) { return m.write_u32(at, v).is_ok(); }

[[nodiscard]] bool r32(Marshaler& m, GuestAddr at, u32& v) {
    const auto r = m.read_u32(at);
    if (r.is_err()) return false;
    v = r.value();
    return true;
}

bool write_rect_round(Marshaler& m, GuestAddr at, const RectF& r) {
    return w32(m, at, static_cast<u32>(round_real_gp(r.X))) && w32(m, at + 4, static_cast<u32>(round_real_gp(r.Y))) &&
           w32(m, at + 8, static_cast<u32>(round_real_gp(r.Width))) &&
           w32(m, at + 12, static_cast<u32>(round_real_gp(r.Height)));
}

RectF bounds_of(const std::vector<PointF>& pts) {
    float minx = pts[0].X, maxx = pts[0].X, miny = pts[0].Y, maxy = pts[0].Y;
    for (const PointF& p : pts) {
        minx = std::min(minx, p.X);
        maxx = std::max(maxx, p.X);
        miny = std::min(miny, p.Y);
        maxy = std::max(maxy, p.Y);
    }
    return {minx, miny, maxx - minx, maxy - miny};
}

/// Store a new brush and hand its handle to the guest.
Status emit(GdiplusManager& mgr, Marshaler& m, GuestAddr out, GpBrush brush) {
    const u64 h = mgr.objects().add(std::move(brush));
    if (!write_handle(m, out, h)) {
        mgr.objects().remove<GpBrush>(h);
        return Status::InvalidParameter;
    }
    return Status::Ok;
}

/// The brush behind argument @p index when it is of @p type. GDI+ does no
/// subtype check (it reads the other class's fields); Vitreo refuses a
/// brush of the wrong type with InvalidParameter (contract doc).
GpBrush* typed(GdiplusManager& mgr, Marshaler& m, usize index, i32 type) {
    GpBrush* b = mgr.objects().get<GpBrush>(handle_arg(m, index));
    return (b != nullptr && b->type == type) ? b : nullptr;
}

/// Why the brush argument @p index was refused: a brush of another subtype
/// is InvalidParameter (see typed()); a live object of another class is
/// ObjectBusy and anything else InvalidParameter, as for every GDI+ object
/// argument (resolve_status, probe-measured by the draw area).
Status why(GdiplusManager& mgr, Marshaler& m, usize index) {
    const u64 h = handle_arg(m, index);
    if (mgr.objects().get<GpBrush>(h) != nullptr) return Status::InvalidParameter;
    return resolve_status(mgr, h, false);
}

// ---- Shared brush-transform exports (texture, line, path gradient) -----------

void def_transforms(GdiplusManager& mgr, HandlerMap& map, i32 type, const char* get, const char* set,
                    const char* reset, const char* multiply, const char* translate, const char* scale,
                    const char* rotate) {
    def(map, get, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        GpMatrix* mx = mgr.objects().get<GpMatrix>(handle_arg(m, 1));
        if (b == nullptr) return why(mgr, m, 0);
        if (mx == nullptr) return resolve_status(mgr, handle_arg(m, 1), false);
        mx->m = b->transform;
        return Status::Ok;
    });
    def(map, set, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        GpMatrix* mx = mgr.objects().get<GpMatrix>(handle_arg(m, 1));
        if (b == nullptr) return why(mgr, m, 0);
        if (mx == nullptr) return resolve_status(mgr, handle_arg(m, 1), false);
        if (!invertible(mx->m)) return Status::InvalidParameter;
        b->transform = mx->m;
        return Status::Ok;
    });
    def(map, reset, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        if (b == nullptr) return why(mgr, m, 0);
        b->transform = Affine{};
        return Status::Ok;
    });
    // NULL matrix: Ok, no change; any order but Prepend appends (probe
    // "texture multiply order2" / "s(2,3) *t(5,6) order2").
    def(map, multiply, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        if (b == nullptr) return why(mgr, m, 0);
        const u64 hm = handle_arg(m, 1);
        if (hm == 0) return Status::Ok;
        GpMatrix* mx = mgr.objects().get<GpMatrix>(hm);
        if (mx == nullptr) return resolve_status(mgr, hm, false);
        if (!invertible(mx->m)) return Status::InvalidParameter;
        b->transform = m.arg32(2) == 0 ? mx->m.then(b->transform) : b->transform.then(mx->m);
        return Status::Ok;
    });
    auto apply = [](GpBrush& b, const Affine& op, u32 order) {
        if (order > 1) return Status::InvalidParameter;
        b.transform = order == 0 ? op.then(b.transform) : b.transform.then(op);
        return Status::Ok;
    };
    def(map, translate, [&mgr, type, apply](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        if (b == nullptr) return why(mgr, m, 0);
        return apply(*b, Affine{1, 0, 0, 1, real_arg(m, 1), real_arg(m, 2)}, m.arg32(3));
    });
    def(map, scale, [&mgr, type, apply](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        if (b == nullptr) return why(mgr, m, 0);
        return apply(*b, Affine{real_arg(m, 1), 0, 0, real_arg(m, 2), 0, 0}, m.arg32(3));
    });
    def(map, rotate, [&mgr, type, apply](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        if (b == nullptr) return why(mgr, m, 0);
        return apply(*b, rotation(real_arg(m, 1)), m.arg32(2));
    });
}

// ---- Shared blend exports (line, path gradient) ------------------------------

/// Sigma (bell) blend: focus/scale in [0,1]; 256 samples per side of the
/// focus of the normal CDF F(z) = (erf(2*sqrt2*(z-0.5))/erf(sqrt2) + 1)/2
/// (fitted to the probe "line sigma" tables, max error 1.5e-5).
Status sigma_blend(GpBrush& b, float focus, float scale) {
    if (!(focus >= 0 && focus <= 1 && scale >= 0 && scale <= 1)) return Status::InvalidParameter;
    const double k = 2.0 * std::sqrt(2.0);
    auto F = [&](double z) { return (std::erf((z - 0.5) * k) / std::erf(k / 2) + 1) / 2; };
    GradientBlend out;
    out.factors.clear();
    out.positions.clear();
    if (focus > 0) {
        for (int i = 0; i <= 255; ++i) {
            out.positions.push_back(focus * static_cast<float>(i) / 255.0f);
            out.factors.push_back(static_cast<float>(scale * F(i / 255.0)));
        }
    }
    if (focus < 1) {
        for (int i = focus > 0 ? 1 : 0; i <= 255; ++i) {
            out.positions.push_back(focus + (1 - focus) * static_cast<float>(i) / 255.0f);
            out.factors.push_back(static_cast<float>(scale * F(1 - i / 255.0)));
        }
    }
    b.blend = std::move(out);
    b.preset = {};
    return Status::Ok;
}

/// Triangular blend (probe "line linear"): (0,0) (scale,focus) (0,1), the
/// end at the focus dropped when the focus is 0 or 1.
Status linear_blend(GpBrush& b, float focus, float scale) {
    if (!(focus >= 0 && focus <= 1 && scale >= 0 && scale <= 1)) return Status::InvalidParameter;
    GradientBlend out;
    if (focus == 0) {
        out.factors = {scale, 0};
        out.positions = {0, 1};
    } else if (focus == 1) {
        out.factors = {0, scale};
        out.positions = {0, 1};
    } else {
        out.factors = {0, scale, 0};
        out.positions = {0, focus, 1};
    }
    b.blend = std::move(out);
    b.preset = {};
    return Status::Ok;
}

void def_blends(GdiplusManager& mgr, HandlerMap& map, i32 type, const char* get_count, const char* get,
                const char* set, const char* get_preset_count, const char* get_preset, const char* set_preset,
                const char* sigma, const char* linear) {
    const bool path = type == BrushTypePathGradient;
    // While interpolation colours are set the blend count reports their
    // count (probe "line preset3 blendcount").
    def(map, get_count, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        const usize n = b->preset.colors.empty() ? b->blend.factors.size() : b->preset.colors.size();
        w32(m, out, static_cast<u32>(n));
        return Status::Ok;
    });
    def(map, get, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        const GuestAddr fac = m.arg_ptr(1), pos = m.arg_ptr(2);
        const i32 count = static_cast<i32>(m.arg32(3));
        if (b == nullptr) return why(mgr, m, 0);
        if (fac == 0 || pos == 0 || count < 1) return Status::InvalidParameter;
        // Windows faults reading the blend while interpolation colours are set.
        if (!b->preset.colors.empty()) return Status::WrongState;
        const usize n = b->blend.factors.size();
        if (static_cast<usize>(count) < n) return Status::InsufficientBuffer;
        for (usize i = 0; i < n; ++i) {
            write_f32(m, fac + i * 4, b->blend.factors[i]);
            write_f32(m, pos + i * 4, n == 1 ? 0.0f : b->blend.positions[i]);
        }
        return Status::Ok;
    });
    def(map, set, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        const GuestAddr fac = m.arg_ptr(1), pos = m.arg_ptr(2);
        const i32 count = static_cast<i32>(m.arg32(3));
        if (b == nullptr) return why(mgr, m, 0);
        if (fac == 0 || pos == 0 || count < 1) return Status::InvalidParameter;
        GradientBlend nb;
        nb.factors.resize(static_cast<usize>(count));
        nb.positions.resize(static_cast<usize>(count));
        for (i32 i = 0; i < count; ++i) {
            if (!read_f32(m, fac + static_cast<u64>(i) * 4, nb.factors[static_cast<usize>(i)]) ||
                !read_f32(m, pos + static_cast<u64>(i) * 4, nb.positions[static_cast<usize>(i)])) {
                return Status::InvalidParameter;
            }
        }
        // Two or more: positions must run from exactly 0 to exactly 1; the
        // middle is not checked, nor are the factors (probe "setblend").
        if (count >= 2 && (nb.positions.front() != 0.0f || nb.positions.back() != 1.0f)) {
            return Status::InvalidParameter;
        }
        if (count == 1) nb.positions[0] = 0.0f;
        b->blend = std::move(nb);
        b->preset = {};
        return Status::Ok;
    });
    def(map, get_preset_count, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(b->preset.colors.size()));
        return Status::Ok;
    });
    def(map, get_preset, [&mgr, type, path](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        const GuestAddr col = m.arg_ptr(1), pos = m.arg_ptr(2);
        const i32 count = static_cast<i32>(m.arg32(3));
        if (b == nullptr) return why(mgr, m, 0);
        if (col == 0 || pos == 0 || count < 2) return Status::InvalidParameter;
        const i32 n = static_cast<i32>(b->preset.colors.size());
        if (n == 0) return Status::GenericError;
        if (!path) {
            if (count < n) return Status::InsufficientBuffer;
            for (i32 i = 0; i < n; ++i) {
                w32(m, col + static_cast<u64>(i) * 4, b->preset.colors[static_cast<usize>(i)]);
                write_f32(m, pos + static_cast<u64>(i) * 4, b->preset.positions[static_cast<usize>(i)]);
            }
            return Status::Ok;
        }
        // Path gradients fill the buffer from its end: slot i receives entry
        // n - count + i; slots before the first entry are left untouched
        // (probe "pg getpreset count2..4").
        for (i32 i = 0; i < count; ++i) {
            const i32 e = n - count + i;
            if (e < 0) continue;
            w32(m, col + static_cast<u64>(i) * 4, b->preset.colors[static_cast<usize>(e)]);
            write_f32(m, pos + static_cast<u64>(i) * 4, b->preset.positions[static_cast<usize>(e)]);
        }
        return Status::Ok;
    });
    def(map, set_preset, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        const GuestAddr col = m.arg_ptr(1), pos = m.arg_ptr(2);
        const i32 count = static_cast<i32>(m.arg32(3));
        if (b == nullptr) return why(mgr, m, 0);
        if (col == 0 || pos == 0 || count < 2) return Status::InvalidParameter;
        GradientPreset np;
        np.colors.resize(static_cast<usize>(count));
        np.positions.resize(static_cast<usize>(count));
        for (i32 i = 0; i < count; ++i) {
            if (!r32(m, col + static_cast<u64>(i) * 4, np.colors[static_cast<usize>(i)]) ||
                !read_f32(m, pos + static_cast<u64>(i) * 4, np.positions[static_cast<usize>(i)])) {
                return Status::InvalidParameter;
            }
        }
        if (np.positions.front() != 0.0f || np.positions.back() != 1.0f) return Status::InvalidParameter;
        b->preset = std::move(np);
        return Status::Ok;
    });
    def(map, sigma, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        if (b == nullptr) return why(mgr, m, 0);
        return sigma_blend(*b, real_arg(m, 1), real_arg(m, 2));
    });
    def(map, linear, [&mgr, type](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, type);
        if (b == nullptr) return why(mgr, m, 0);
        return linear_blend(*b, real_arg(m, 1), real_arg(m, 2));
    });
}

// ---- Texture creation ----------------------------------------------------------

/// Build a texture brush from @p image_h cropped to the GpRound()ed
/// rect (null = whole image). Out-of-image or empty rects and invalid wrap
/// modes are OutOfMemory (probe "texture2", "texture wrap").
Status make_texture(GdiplusManager& mgr, Marshaler& m, u64 image_h, i32 wrap, const RectF* src,
                    u64 attributes, GuestAddr out) {
    if (out == 0) return Status::InvalidParameter;
    const GpImage* image = mgr.objects().get<GpImage>(image_h);
    if (image == nullptr) return resolve_status(mgr, image_h, false);
    if (wrap < WrapModeTile || wrap > WrapModeClamp) return Status::OutOfMemory;
    const GpImageAttributes* ia = nullptr;
    if (attributes != 0) {
        ia = mgr.objects().get<GpImageAttributes>(attributes);
        if (ia == nullptr) return resolve_status(mgr, attributes, false);
        // The IA forms take the wrap mode from the attributes (probe
        // "textureIA fresh-attr wrap 4", "attr-wrap3 wrap 3").
        wrap = ia->wrap_mode;
    }
    if (image->type != ImageTypeBitmap) {
        log_unimplemented("GdipCreateTexture", "metafile image");
        return Status::NotImplemented;
    }
    RenderPixels px;
    if (!image_to_render(m, *image, px)) {
        log_unimplemented("GdipCreateTexture", "image pixel format without a render conversion");
        return Status::NotImplemented;
    }
    i32 sx = 0, sy = 0, sw = px.width, sh = px.height;
    if (src != nullptr) {
        sx = round_real_gp(src->X);
        sy = round_real_gp(src->Y);
        sw = round_real_gp(src->Width);
        sh = round_real_gp(src->Height);
        if (sx < 0 || sy < 0 || sw <= 0 || sh <= 0 || sx + sw > px.width || sy + sh > px.height) {
            return Status::OutOfMemory;
        }
    }
    auto tex = std::make_shared<GpImage>();
    tex->type = ImageTypeBitmap;
    tex->width = sw;
    tex->height = sh;
    tex->pixel_format = PixelFormat32bppPARGB;  // probe "texture image ... pf=e200b"
    tex->dpi_x = image->dpi_x;
    tex->dpi_y = image->dpi_y;
    std::memcpy(tex->raw_format, kImageFormatMemoryBMP.data(), sizeof(tex->raw_format));
    tex->stride = sw * 4;
    tex->bits.resize(static_cast<usize>(sw) * static_cast<usize>(sh) * 4u);
    for (i32 y = 0; y < sh; ++y) {
        std::memcpy(&tex->bits[static_cast<usize>(y) * static_cast<usize>(sw) * 4u],
                    &px.bgra[(static_cast<usize>(sy + y) * static_cast<usize>(px.width) + static_cast<usize>(sx)) * 4u],
                    static_cast<usize>(sw) * 4u);
    }
    if (ia != nullptr) {
        // The attributes' colour adjustments are baked into the brush's
        // pixels, and GetTextureImage reports the adjusted pixels (probe
        // "textureIAI matrix ... image px21=ff118080"). The texture image is
        // drawn as a bitmap, so the Bitmap category applies (falling back
        // to Default as the image area resolves it).
        constexpr i32 kColorAdjustTypeBitmap = 1;
        const ColorAdjust& adj = effective_adjust(*ia, kColorAdjustTypeBitmap);
        const usize n = static_cast<usize>(sw) * static_cast<usize>(sh);
        std::vector<u32> argb(n);
        for (usize i = 0; i < n; ++i) {
            const u8* p = &tex->bits[i * 4];
            const u32 a = p[3];
            auto un = [&](u8 c) { return a == 0 ? 0u : std::min(255u, (c * 255u + a / 2) / a); };
            argb[i] = a << 24 | un(p[2]) << 16 | un(p[1]) << 8 | un(p[0]);
        }
        apply_color_adjust(adj, argb.data(), n);
        for (usize i = 0; i < n; ++i) store(premul(argb[i]), &tex->bits[i * 4]);
    }
    GpBrush b;
    b.type = BrushTypeTextureFill;
    b.wrap = wrap;
    b.texture = std::move(tex);
    return emit(mgr, m, out, std::move(b));
}

// ---- Linear gradient creation ----------------------------------------------------

/// GdipCreateLineBrush(I): the rect is the points' bounds, a zero extent
/// replaced by the other centred on the line; the transform is the true
/// (non-scalable) angle of p1->p2 (probe "line x,y-x,y" rows).
Status make_line_from_points(GdiplusManager& mgr, Marshaler& m, PointF p1, PointF p2, u32 c1, u32 c2,
                             i32 wrap, GuestAddr out) {
    if (out == 0) return Status::InvalidParameter;
    if (wrap == WrapModeClamp) return Status::InvalidParameter;
    if (p1.X == p2.X && p1.Y == p2.Y) return Status::OutOfMemory;
    RectF r{std::min(p1.X, p2.X), std::min(p1.Y, p2.Y), std::fabs(p2.X - p1.X), std::fabs(p2.Y - p1.Y)};
    if (r.Width == 0) {
        r.Width = r.Height;
        r.X -= r.Height / 2;
    } else if (r.Height == 0) {
        r.Height = r.Width;
        r.Y -= r.Width / 2;
    }
    const float angle = static_cast<float>(std::atan2(static_cast<double>(p2.Y) - p1.Y,
                                                      static_cast<double>(p2.X) - p1.X) *
                                           180.0 / 3.14159265358979323846);
    GpBrush b;
    b.type = BrushTypeLinearGradient;
    b.rect = r;
    b.line_colors[0] = c1;
    b.line_colors[1] = c2;
    b.wrap = wrap;
    b.transform = angle_transform(r, angle, false);
    return emit(mgr, m, out, std::move(b));
}

Status make_line_from_rect(GdiplusManager& mgr, Marshaler& m, const RectF& r, u32 c1, u32 c2, float angle,
                           bool scalable, i32 wrap, GuestAddr out) {
    if (out == 0) return Status::InvalidParameter;
    if (wrap == WrapModeClamp) return Status::InvalidParameter;
    if (r.Width == 0 || r.Height == 0) return Status::OutOfMemory;
    GpBrush b;
    b.type = BrushTypeLinearGradient;
    b.rect = r;
    b.line_colors[0] = c1;
    b.line_colors[1] = c2;
    b.wrap = wrap;
    b.transform = angle_transform(r, angle, scalable);
    return emit(mgr, m, out, std::move(b));
}

/// LinearGradientMode -> the angle GDI+ uses (scalable): Horizontal 0,
/// Vertical 90, ForwardDiagonal 45, BackwardDiagonal 135 (probe "rect mode").
bool mode_angle(u32 mode, float& angle) {
    static constexpr float kAngles[4] = {0, 90, 45, 135};
    if (mode > 3) return false;
    angle = kAngles[mode];
    return true;
}

// ---- Path gradient creation ------------------------------------------------------

Status make_path_gradient(GdiplusManager& mgr, Marshaler& m, std::vector<PointF> pts, std::vector<u8> types,
                          i32 wrap, bool from_path, GuestAddr out) {
    if (out == 0) return Status::InvalidParameter;
    if (pts.size() < (from_path ? 1u : 3u)) return Status::OutOfMemory;
    if (wrap < WrapModeTile || wrap > WrapModeClamp) return Status::OutOfMemory;
    const RectF r = bounds_of(pts);
    if (r.Width == 0 || r.Height == 0) return Status::OutOfMemory;  // probe "pg collinear" / "pg diag"
    GpBrush b;
    b.type = BrushTypePathGradient;
    b.rect = r;
    b.wrap = wrap;
    // Centre: the plain average of every point, Bézier control points
    // included, summed in REAL (probe "pg frompath rect+ellipse" 20.58823).
    float sx = 0, sy = 0;
    for (const PointF& p : pts) {
        sx += p.X;
        sy += p.Y;
    }
    b.center = {sx / static_cast<float>(pts.size()), sy / static_cast<float>(pts.size())};
    b.center_color = from_path ? 0xFFFFFFFFu : 0xFF000000u;
    b.surround = {0xFFFFFFFFu};
    b.path_points = std::move(pts);
    b.path_types = std::move(types);
    return emit(mgr, m, out, std::move(b));
}

}  // namespace

void register_pen_brush(GdiplusManager& mgr, HandlerMap& map) {
    auto& objs = mgr.objects();

    // ---- Brush --------------------------------------------------------------
    def(map, "GdipCloneBrush", [&mgr](Marshaler& m) {
        GpBrush* b = mgr.objects().get<GpBrush>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        return emit(mgr, m, out, *b);
    });
    def(map, "GdipDeleteBrush", [&mgr, &objs](Marshaler& m) {
        return objs.remove<GpBrush>(handle_arg(m, 0)) ? Status::Ok : why(mgr, m, 0);
    });
    def(map, "GdipGetBrushType", [&mgr, &objs](Marshaler& m) {
        GpBrush* b = objs.get<GpBrush>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(b->type));
        return Status::Ok;
    });

    // ---- SolidFill ----------------------------------------------------------
    def(map, "GdipCreateSolidFill", [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return Status::InvalidParameter;
        GpBrush b;
        b.type = BrushTypeSolidColor;
        b.color = m.arg32(0);
        return emit(mgr, m, out, std::move(b));
    });
    def(map, "GdipSetSolidFillColor", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeSolidColor);
        if (b == nullptr) return why(mgr, m, 0);
        b->color = m.arg32(1);
        return Status::Ok;
    });
    def(map, "GdipGetSolidFillColor", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeSolidColor);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, b->color);
        return Status::Ok;
    });

    // ---- HatchBrush -----------------------------------------------------------
    def(map, "GdipCreateHatchBrush", [&mgr](Marshaler& m) {
        const i32 style = static_cast<i32>(m.arg32(0));
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0 || style < 0 || style > kHatchStyleMax) return Status::InvalidParameter;
        GpBrush b;
        b.type = BrushTypeHatchFill;
        b.hatch_style = style;
        b.color = m.arg32(1);
        b.hatch_back = m.arg32(2);
        return emit(mgr, m, out, std::move(b));
    });
    def(map, "GdipGetHatchStyle", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeHatchFill);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(b->hatch_style));
        return Status::Ok;
    });
    def(map, "GdipGetHatchForegroundColor", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeHatchFill);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, b->color);
        return Status::Ok;
    });
    def(map, "GdipGetHatchBackgroundColor", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeHatchFill);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, b->hatch_back);
        return Status::Ok;
    });

    // ---- TextureBrush -----------------------------------------------------------
    def(map, "GdipCreateTexture", [&mgr](Marshaler& m) {
        return make_texture(mgr, m, handle_arg(m, 0), static_cast<i32>(m.arg32(1)), nullptr, 0, m.arg_ptr(2));
    });
    def(map, "GdipCreateTexture2", [&mgr](Marshaler& m) {
        const RectF r{real_arg(m, 2), real_arg(m, 3), real_arg(m, 4), real_arg(m, 5)};
        return make_texture(mgr, m, handle_arg(m, 0), static_cast<i32>(m.arg32(1)), &r, 0, m.arg_ptr(6));
    });
    def(map, "GdipCreateTexture2I", [&mgr](Marshaler& m) {
        const RectF r{static_cast<float>(static_cast<i32>(m.arg32(2))), static_cast<float>(static_cast<i32>(m.arg32(3))),
                      static_cast<float>(static_cast<i32>(m.arg32(4))), static_cast<float>(static_cast<i32>(m.arg32(5)))};
        return make_texture(mgr, m, handle_arg(m, 0), static_cast<i32>(m.arg32(1)), &r, 0, m.arg_ptr(6));
    });
    // The IA forms take the wrap mode from the attributes; NULL attributes
    // mean WrapModeTile (probe "textureIA null-attr").
    def(map, "GdipCreateTextureIA", [&mgr](Marshaler& m) {
        const RectF r{real_arg(m, 2), real_arg(m, 3), real_arg(m, 4), real_arg(m, 5)};
        return make_texture(mgr, m, handle_arg(m, 0), WrapModeTile, &r, handle_arg(m, 1), m.arg_ptr(6));
    });
    def(map, "GdipCreateTextureIAI", [&mgr](Marshaler& m) {
        const RectF r{static_cast<float>(static_cast<i32>(m.arg32(2))), static_cast<float>(static_cast<i32>(m.arg32(3))),
                      static_cast<float>(static_cast<i32>(m.arg32(4))), static_cast<float>(static_cast<i32>(m.arg32(5)))};
        return make_texture(mgr, m, handle_arg(m, 0), WrapModeTile, &r, handle_arg(m, 1), m.arg_ptr(6));
    });
    def(map, "GdipGetTextureImage", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeTextureFill);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0 || !b->texture) return Status::InvalidParameter;
        // A new image per call (probe "texture image twice distinct=1").
        const u64 h = mgr.objects().add(GpImage(*b->texture));
        if (!write_handle(m, out, h)) {
            mgr.objects().remove<GpImage>(h);
            return Status::InvalidParameter;
        }
        return Status::Ok;
    });
    // Out-of-range wrap modes are accepted and ignored (probe "texture
    // after set5 wrap 4").
    def(map, "GdipSetTextureWrapMode", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeTextureFill);
        if (b == nullptr) return why(mgr, m, 0);
        const i32 w = static_cast<i32>(m.arg32(1));
        if (w >= WrapModeTile && w <= WrapModeClamp) b->wrap = w;
        return Status::Ok;
    });
    def(map, "GdipGetTextureWrapMode", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeTextureFill);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(b->wrap));
        return Status::Ok;
    });
    def_transforms(mgr, map, BrushTypeTextureFill, "GdipGetTextureTransform", "GdipSetTextureTransform",
                   "GdipResetTextureTransform", "GdipMultiplyTextureTransform", "GdipTranslateTextureTransform",
                   "GdipScaleTextureTransform", "GdipRotateTextureTransform");

    // ---- LinearGradientBrush ------------------------------------------------------
    def(map, "GdipCreateLineBrush", [&mgr](Marshaler& m) {
        const GuestAddr a = m.arg_ptr(0), b = m.arg_ptr(1);
        std::vector<PointF> p1, p2;
        if (a == 0 || b == 0 || !read_points(m, a, 1, p1) || !read_points(m, b, 1, p2)) {
            return Status::InvalidParameter;
        }
        return make_line_from_points(mgr, m, p1[0], p2[0], m.arg32(2), m.arg32(3),
                                     static_cast<i32>(m.arg32(4)), m.arg_ptr(5));
    });
    def(map, "GdipCreateLineBrushI", [&mgr](Marshaler& m) {
        const GuestAddr a = m.arg_ptr(0), b = m.arg_ptr(1);
        std::vector<PointF> p1, p2;
        if (a == 0 || b == 0 || !read_points_i(m, a, 1, p1) || !read_points_i(m, b, 1, p2)) {
            return Status::InvalidParameter;
        }
        return make_line_from_points(mgr, m, p1[0], p2[0], m.arg32(2), m.arg32(3),
                                     static_cast<i32>(m.arg32(4)), m.arg_ptr(5));
    });
    // An unknown LinearGradientMode is OutOfMemory (probe "rect mode -1/4").
    auto from_rect = [&mgr](Marshaler& m, bool ints) {
        const GuestAddr rp = m.arg_ptr(0);
        RectF r;
        if (rp == 0 || !(ints ? read_rect_i(m, rp, r) : read_rect(m, rp, r))) return Status::InvalidParameter;
        float angle = 0;
        if (m.arg_ptr(5) == 0) return Status::InvalidParameter;
        if (static_cast<i32>(m.arg32(4)) == WrapModeClamp) return Status::InvalidParameter;
        if (!mode_angle(m.arg32(3), angle)) return Status::OutOfMemory;
        return make_line_from_rect(mgr, m, r, m.arg32(1), m.arg32(2), angle, true,
                                   static_cast<i32>(m.arg32(4)), m.arg_ptr(5));
    };
    def(map, "GdipCreateLineBrushFromRect", [from_rect](Marshaler& m) { return from_rect(m, false); });
    def(map, "GdipCreateLineBrushFromRectI", [from_rect](Marshaler& m) { return from_rect(m, true); });
    auto with_angle = [&mgr](Marshaler& m, bool ints) {
        const GuestAddr rp = m.arg_ptr(0);
        RectF r;
        if (rp == 0 || !(ints ? read_rect_i(m, rp, r) : read_rect(m, rp, r))) return Status::InvalidParameter;
        return make_line_from_rect(mgr, m, r, m.arg32(1), m.arg32(2), real_arg(m, 3), m.arg32(4) != 0,
                                   static_cast<i32>(m.arg32(5)), m.arg_ptr(6));
    };
    def(map, "GdipCreateLineBrushFromRectWithAngle", [with_angle](Marshaler& m) { return with_angle(m, false); });
    def(map, "GdipCreateLineBrushFromRectWithAngleI", [with_angle](Marshaler& m) { return with_angle(m, true); });
    def(map, "GdipSetLineColors", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeLinearGradient);
        if (b == nullptr) return why(mgr, m, 0);
        b->line_colors[0] = m.arg32(1);
        b->line_colors[1] = m.arg32(2);
        return Status::Ok;
    });
    def(map, "GdipGetLineColors", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeLinearGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, b->line_colors[0]);
        w32(m, out + 4, b->line_colors[1]);
        return Status::Ok;
    });
    def(map, "GdipGetLineRect", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeLinearGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        write_rect(m, out, b->rect);
        return Status::Ok;
    });
    def(map, "GdipGetLineRectI", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeLinearGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        write_rect_round(m, out, b->rect);
        return Status::Ok;
    });
    def(map, "GdipSetLineGammaCorrection", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeLinearGradient);
        if (b == nullptr) return why(mgr, m, 0);
        b->gamma = static_cast<i32>(m.arg32(1));
        return Status::Ok;
    });
    def(map, "GdipGetLineGammaCorrection", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeLinearGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(b->gamma));
        return Status::Ok;
    });
    // Clamp is refused; other unknown values are accepted and ignored
    // (probe "line setwrap clamp st=2", "line after set5 wrap 3").
    def(map, "GdipSetLineWrapMode", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeLinearGradient);
        if (b == nullptr) return why(mgr, m, 0);
        const i32 w = static_cast<i32>(m.arg32(1));
        if (w == WrapModeClamp) return Status::InvalidParameter;
        if (w >= WrapModeTile && w < WrapModeClamp) b->wrap = w;
        return Status::Ok;
    });
    def(map, "GdipGetLineWrapMode", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypeLinearGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(b->wrap));
        return Status::Ok;
    });
    def_blends(mgr, map, BrushTypeLinearGradient, "GdipGetLineBlendCount", "GdipGetLineBlend", "GdipSetLineBlend",
               "GdipGetLinePresetBlendCount", "GdipGetLinePresetBlend", "GdipSetLinePresetBlend",
               "GdipSetLineSigmaBlend", "GdipSetLineLinearBlend");
    def_transforms(mgr, map, BrushTypeLinearGradient, "GdipGetLineTransform", "GdipSetLineTransform",
                   "GdipResetLineTransform", "GdipMultiplyLineTransform", "GdipTranslateLineTransform",
                   "GdipScaleLineTransform", "GdipRotateLineTransform");

    // ---- PathGradientBrush ------------------------------------------------------
    // NULL points: OutOfMemory for the REAL form, InvalidParameter for the
    // INT form (probe "pg null pts st=3", "pgI ... null st=2").
    def(map, "GdipCreatePathGradient", [&mgr](Marshaler& m) {
        const GuestAddr pts = m.arg_ptr(0);
        const i32 count = static_cast<i32>(m.arg32(1));
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0) return Status::InvalidParameter;
        if (pts == 0 || count < 3) return Status::OutOfMemory;
        std::vector<PointF> p;
        if (!read_points(m, pts, count, p)) return Status::InvalidParameter;
        return make_path_gradient(mgr, m, std::move(p), std::vector<u8>(static_cast<usize>(count), PathPointTypeLine),
                                  static_cast<i32>(m.arg32(2)), false, out);
    });
    def(map, "GdipCreatePathGradientI", [&mgr](Marshaler& m) {
        const GuestAddr pts = m.arg_ptr(0);
        const i32 count = static_cast<i32>(m.arg32(1));
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0 || pts == 0) return Status::InvalidParameter;
        if (count < 3) return Status::OutOfMemory;
        std::vector<PointF> p;
        if (!read_points_i(m, pts, count, p)) return Status::InvalidParameter;
        return make_path_gradient(mgr, m, std::move(p), std::vector<u8>(static_cast<usize>(count), PathPointTypeLine),
                                  static_cast<i32>(m.arg32(2)), false, out);
    });
    // From a path: centre colour white, WrapModeClamp; a NULL or empty path
    // is OutOfMemory (probe "pg frompath").
    def(map, "GdipCreatePathGradientFromPath", [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return Status::InvalidParameter;
        const u64 hp = handle_arg(m, 0);
        const GpPath* path = mgr.objects().get<GpPath>(hp);
        if (path == nullptr && hp != 0) {
            const Status s = resolve_status(mgr, hp, false);
            if (s == Status::ObjectBusy) return s;
        }
        if (path == nullptr || path->points.empty()) return Status::OutOfMemory;
        return make_path_gradient(mgr, m, path->points, path->types, WrapModeClamp, true, out);
    });
    def(map, "GdipGetPathGradientCenterColor", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, b->center_color);
        return Status::Ok;
    });
    def(map, "GdipSetPathGradientCenterColor", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        if (b == nullptr) return why(mgr, m, 0);
        b->center_color = m.arg32(1);
        return Status::Ok;
    });
    // Writes one colour per boundary point (the last set colour repeating)
    // and stores back the count that was set; a buffer smaller than the
    // point count is InvalidParameter (probe "pg stored2 get k1..k5").
    def(map, "GdipGetPathGradientSurroundColorsWithCount", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr colors = m.arg_ptr(1), count_p = m.arg_ptr(2);
        if (b == nullptr) return why(mgr, m, 0);
        if (colors == 0 || count_p == 0) return Status::InvalidParameter;
        u32 count = 0;
        if (!r32(m, count_p, count)) return Status::InvalidParameter;
        const usize n = b->path_points.size();
        if (static_cast<i32>(count) < static_cast<i32>(n)) return Status::InvalidParameter;
        for (usize i = 0; i < n; ++i) {
            w32(m, colors + i * 4, i < b->surround.size() ? b->surround[i] : b->surround.back());
        }
        w32(m, count_p, static_cast<u32>(b->surround.size()));
        return Status::Ok;
    });
    def(map, "GdipSetPathGradientSurroundColorsWithCount", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr colors = m.arg_ptr(1), count_p = m.arg_ptr(2);
        if (b == nullptr) return why(mgr, m, 0);
        if (colors == 0 || count_p == 0) return Status::InvalidParameter;
        u32 count = 0;
        if (!r32(m, count_p, count)) return Status::InvalidParameter;
        const i32 c = static_cast<i32>(count);
        if (c < 1 || c > static_cast<i32>(b->path_points.size())) return Status::InvalidParameter;
        std::vector<u32> s(static_cast<usize>(c));
        for (i32 i = 0; i < c; ++i) {
            if (!r32(m, colors + static_cast<u64>(i) * 4, s[static_cast<usize>(i)])) return Status::InvalidParameter;
        }
        b->surround = std::move(s);
        return Status::Ok;
    });
    // GDI+ implements neither (probe "pg getpath st=6 setpath st=6", with
    // NULL arguments too): this is the Windows contract, not a gap.
    def(map, "GdipGetPathGradientPath", [](Marshaler&) { return Status::NotImplemented; });
    def(map, "GdipSetPathGradientPath", [](Marshaler&) { return Status::NotImplemented; });
    def(map, "GdipGetPathGradientCenterPoint", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        write_points(m, out, {b->center});
        return Status::Ok;
    });
    def(map, "GdipGetPathGradientCenterPointI", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(round_real_gp(b->center.X)));
        w32(m, out + 4, static_cast<u32>(round_real_gp(b->center.Y)));
        return Status::Ok;
    });
    def(map, "GdipSetPathGradientCenterPoint", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr in = m.arg_ptr(1);
        std::vector<PointF> p;
        if (b == nullptr) return why(mgr, m, 0);
        if (in == 0 || !read_points(m, in, 1, p)) return Status::InvalidParameter;
        b->center = p[0];
        return Status::Ok;
    });
    def(map, "GdipSetPathGradientCenterPointI", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr in = m.arg_ptr(1);
        std::vector<PointF> p;
        if (b == nullptr) return why(mgr, m, 0);
        if (in == 0 || !read_points_i(m, in, 1, p)) return Status::InvalidParameter;
        b->center = p[0];
        return Status::Ok;
    });
    def(map, "GdipGetPathGradientRect", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        write_rect(m, out, b->rect);
        return Status::Ok;
    });
    def(map, "GdipGetPathGradientRectI", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        write_rect_round(m, out, b->rect);
        return Status::Ok;
    });
    def(map, "GdipGetPathGradientPointCount", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(b->path_points.size()));
        return Status::Ok;
    });
    def(map, "GdipGetPathGradientSurroundColorCount", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(b->path_points.size()));  // probe "pg surround two count 4"
        return Status::Ok;
    });
    def(map, "GdipSetPathGradientGammaCorrection", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        if (b == nullptr) return why(mgr, m, 0);
        b->gamma = static_cast<i32>(m.arg32(1));
        return Status::Ok;
    });
    def(map, "GdipGetPathGradientGammaCorrection", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(b->gamma));
        return Status::Ok;
    });
    def(map, "GdipGetPathGradientWrapMode", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr out = m.arg_ptr(1);
        if (b == nullptr) return why(mgr, m, 0);
        if (out == 0) return Status::InvalidParameter;
        w32(m, out, static_cast<u32>(b->wrap));
        return Status::Ok;
    });
    def(map, "GdipSetPathGradientWrapMode", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        if (b == nullptr) return why(mgr, m, 0);
        const i32 w = static_cast<i32>(m.arg32(1));
        if (w >= WrapModeTile && w <= WrapModeClamp) b->wrap = w;  // others ignored (probe)
        return Status::Ok;
    });
    // Focus scales are stored unchecked (probe "pg focus out-of-range").
    def(map, "GdipGetPathGradientFocusScales", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        const GuestAddr x = m.arg_ptr(1), y = m.arg_ptr(2);
        if (b == nullptr) return why(mgr, m, 0);
        if (x == 0 || y == 0) return Status::InvalidParameter;
        write_f32(m, x, b->focus.X);
        write_f32(m, y, b->focus.Y);
        return Status::Ok;
    });
    def(map, "GdipSetPathGradientFocusScales", [&mgr](Marshaler& m) {
        GpBrush* b = typed(mgr, m, 0, BrushTypePathGradient);
        if (b == nullptr) return why(mgr, m, 0);
        b->focus = {real_arg(m, 1), real_arg(m, 2)};
        return Status::Ok;
    });
    def_blends(mgr, map, BrushTypePathGradient, "GdipGetPathGradientBlendCount", "GdipGetPathGradientBlend",
               "GdipSetPathGradientBlend", "GdipGetPathGradientPresetBlendCount", "GdipGetPathGradientPresetBlend",
               "GdipSetPathGradientPresetBlend", "GdipSetPathGradientSigmaBlend", "GdipSetPathGradientLinearBlend");
    def_transforms(mgr, map, BrushTypePathGradient, "GdipGetPathGradientTransform", "GdipSetPathGradientTransform",
                   "GdipResetPathGradientTransform", "GdipMultiplyPathGradientTransform",
                   "GdipTranslatePathGradientTransform", "GdipScalePathGradientTransform",
                   "GdipRotatePathGradientTransform");
}

}  // namespace vitreo::winapi::gdip
