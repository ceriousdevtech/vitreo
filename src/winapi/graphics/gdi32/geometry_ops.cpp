// SPDX-License-Identifier: LGPL-2.1-only

/// @file geometry_ops.cpp
/// @brief GDI paths, polygons, shape regions, coordinate transforms and
///        device capabilities.
///
/// Measured by tools/oracle_probe/s3gdi_region_probe.c on Windows 11:
///  - Polygon interiors (and paths, and regions built from shapes) cover the
///    pixels whose integer point (x, y) lies inside the shape, with left and
///    top edges inclusive and right and bottom edges exclusive: the triangle
///    (5,5) (25,5) (15,25) covers 5..24 on row 5, 6..24 on row 6, 6..23 on
///    row 7 ("polygon tri nullpen"). ALTERNATE and WINDING are the even-odd
///    and non-zero rules ("star alternate" 205 pixels, "star winding" 295).
///  - A path keeps points in device units; FillPath, StrokePath,
///    StrokeAndFillPath and SelectClipPath consume it; an open figure makes
///    FillPath fail; an empty closed path fills nothing and succeeds.
///  - WidenPath replaces the path with the area its pen would paint: for a
///    round pen the set of points within half the pen width of the path
///    ("fill widened": a 5 px pen over (10,10)-(40,10) covers rows 8..12,
///    x 9..41 on row 8 and 8..42 on rows 9..11).
///  - DPtoLP / LPtoDP apply the world and page transforms and round halves to
///    even ("dptolp -> -18,-29 32,4" for -17.5 and 32.5).
///  - Most failures leave the last error untouched.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/graphics/gdi32/bitmap_codec.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace vitreo::winapi {

namespace {

constexpr u8 PT_CLOSEFIGURE = 1, PT_LINETO = 2, PT_BEZIERTO = 4, PT_MOVETO = 6;

struct Pt {
    double x;
    double y;
};
using Poly = std::vector<Pt>;

struct Figure {
    Poly pts;
    bool closed{false};
};

/// Round half to even, the FPU's default mode GDI's conversions use.
i32 round_even(double v) {
    const double r = std::nearbyint(v);
    return static_cast<i32>(r);
}

void flatten_bezier(Pt p0, Pt p1, Pt p2, Pt p3, Poly& out, int depth) {
    // Control points within half a pixel of the chord are flat: the
    // tolerance at which fills of flattened curves match Windows ("fill
    // ellipse path" 418 pixels, "fillpath" 469; a tenth of a pixel gives 434
    // and 473).
    const double dx = p3.x - p0.x, dy = p3.y - p0.y;
    const double len = std::hypot(dx, dy);
    auto dist = [&](Pt p) {
        if (len < 1e-9) return std::hypot(p.x - p0.x, p.y - p0.y);
        return std::abs((p.x - p0.x) * dy - (p.y - p0.y) * dx) / len;
    };
    if (depth >= 16 || std::max(dist(p1), dist(p2)) <= 0.5) {
        out.push_back(p3);
        return;
    }
    const Pt a{(p0.x + p1.x) / 2, (p0.y + p1.y) / 2}, b{(p1.x + p2.x) / 2, (p1.y + p2.y) / 2},
        c{(p2.x + p3.x) / 2, (p2.y + p3.y) / 2};
    const Pt d{(a.x + b.x) / 2, (a.y + b.y) / 2}, e{(b.x + c.x) / 2, (b.y + c.y) / 2};
    const Pt m{(d.x + e.x) / 2, (d.y + e.y) / 2};
    flatten_bezier(p0, a, d, m, out, depth + 1);
    flatten_bezier(m, e, c, p3, out, depth + 1);
}

/// A path's figures as polylines, Béziers flattened.
std::vector<Figure> figures_of(const std::vector<GdiManager::PathPoint>& pts) {
    std::vector<Figure> figures;
    for (usize i = 0; i < pts.size(); ++i) {
        const auto& p = pts[i];
        const u8 kind = static_cast<u8>(p.type & ~PT_CLOSEFIGURE);
        if (kind == PT_MOVETO || figures.empty()) {
            figures.push_back({});
            figures.back().pts.push_back({p.x, p.y});
        } else if (kind == PT_LINETO) {
            figures.back().pts.push_back({p.x, p.y});
        } else if (kind == PT_BEZIERTO && i + 2 < pts.size()) {
            const Pt p0 = figures.back().pts.back();
            flatten_bezier(p0, {pts[i].x, pts[i].y}, {pts[i + 1].x, pts[i + 1].y}, {pts[i + 2].x, pts[i + 2].y},
                           figures.back().pts, 0);
            i += 2;
            if (pts[i].type & PT_CLOSEFIGURE) figures.back().closed = true;
            continue;
        }
        if (p.type & PT_CLOSEFIGURE) figures.back().closed = true;
    }
    return figures;
}

/// Pixels covered by polygons: the integer point (x, y) inside, left/top
/// edges inclusive, right/bottom exclusive.
struct Coverage {
    i32 y0{0};
    std::vector<std::vector<std::pair<i32, i32>>> rows;  ///< [x0, x1) spans per row
    [[nodiscard]] bool empty() const {
        for (const auto& r : rows) {
            if (!r.empty()) return false;
        }
        return true;
    }
};

Coverage polygon_coverage(const std::vector<Poly>& polys, i32 fill_mode) {
    Coverage out;
    double ymin = 1e300, ymax = -1e300;
    for (const auto& p : polys) {
        for (const auto& q : p) {
            ymin = std::min(ymin, q.y);
            ymax = std::max(ymax, q.y);
        }
    }
    if (ymin > ymax) return out;
    const i32 y0 = static_cast<i32>(std::ceil(ymin));
    const i32 y1 = static_cast<i32>(std::ceil(ymax));
    out.y0 = y0;
    if (y1 <= y0 || y1 - y0 > 65536) return out;
    out.rows.resize(static_cast<usize>(y1 - y0));
    struct Cross {
        double x;
        i32 dir;
    };
    std::vector<Cross> xs;
    for (i32 y = y0; y < y1; ++y) {
        xs.clear();
        for (const auto& p : polys) {
            const usize n = p.size();
            if (n < 2) continue;
            for (usize i = 0; i < n; ++i) {
                const Pt a = p[i], b = p[(i + 1) % n];
                if (a.y == b.y) continue;
                const double lo = std::min(a.y, b.y), hi = std::max(a.y, b.y);
                if (y < lo || y >= hi) continue;
                const double x = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
                xs.push_back({x, b.y > a.y ? 1 : -1});
            }
        }
        std::sort(xs.begin(), xs.end(), [](const Cross& l, const Cross& r) { return l.x < r.x; });
        auto& row = out.rows[static_cast<usize>(y - y0)];
        auto add = [&](double xa, double xb) {
            const i32 a = static_cast<i32>(std::ceil(xa - 1e-9));
            const i32 b = static_cast<i32>(std::ceil(xb - 1e-9));
            if (b <= a) return;
            if (!row.empty() && row.back().second >= a) row.back().second = std::max(row.back().second, b);
            else row.push_back({a, b});
        };
        if (fill_mode == 2) {  // WINDING
            i32 winding = 0;
            for (usize i = 0; i + 1 < xs.size(); ++i) {
                winding += xs[i].dir;
                if (winding != 0) add(xs[i].x, xs[i + 1].x);
            }
        } else {
            for (usize i = 0; i + 1 < xs.size(); i += 2) add(xs[i].x, xs[i + 1].x);
        }
    }
    return out;
}

/// Coverage as a region: rows with identical spans merged into bands.
ClipRect coverage_region(const Coverage& c) {
    ClipRect out;
    out.valid = true;
    std::vector<std::array<i32, 4>> rects;
    usize i = 0;
    while (i < c.rows.size()) {
        usize j = i + 1;
        while (j < c.rows.size() && c.rows[j] == c.rows[i]) ++j;
        for (const auto& s : c.rows[i]) {
            rects.push_back({s.first, c.y0 + static_cast<i32>(i), s.second, c.y0 + static_cast<i32>(j)});
        }
        i = j;
    }
    if (rects.empty()) return out;  // 0,0,0,0: a NULLREGION
    out.left = rects[0][0];
    out.top = rects[0][1];
    out.right = rects[0][2];
    out.bottom = rects[0][3];
    for (const auto& r : rects) {
        out.left = std::min(out.left, r[0]);
        out.top = std::min(out.top, r[1]);
        out.right = std::max(out.right, r[2]);
        out.bottom = std::max(out.bottom, r[3]);
    }
    if (rects.size() > 1) out.rectangles = std::move(rects);
    return out;
}

/// The points within @p radius of a polyline, as coverage (WidenPath's round
/// pen).
Coverage widened_coverage(const std::vector<Figure>& figures, double radius) {
    Coverage out;
    double ymin = 1e300, ymax = -1e300, xmin = 1e300, xmax = -1e300;
    for (const auto& f : figures) {
        for (const auto& p : f.pts) {
            ymin = std::min(ymin, p.y - radius);
            ymax = std::max(ymax, p.y + radius);
            xmin = std::min(xmin, p.x - radius);
            xmax = std::max(xmax, p.x + radius);
        }
    }
    if (ymin > ymax) return out;
    const i32 y0 = static_cast<i32>(std::ceil(ymin)), y1 = static_cast<i32>(std::ceil(ymax));
    const i32 x0 = static_cast<i32>(std::ceil(xmin)), x1 = static_cast<i32>(std::ceil(xmax));
    if (y1 - y0 > 16384 || x1 - x0 > 16384) return out;
    out.y0 = y0;
    out.rows.resize(static_cast<usize>(std::max(0, y1 - y0)));
    auto seg_dist2 = [](Pt p, Pt a, Pt b) {
        const double vx = b.x - a.x, vy = b.y - a.y;
        const double len2 = vx * vx + vy * vy;
        double t = len2 > 0 ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const double dx = p.x - (a.x + t * vx), dy = p.y - (a.y + t * vy);
        return dx * dx + dy * dy;
    };
    const double r2 = radius * radius;
    for (i32 y = y0; y < y1; ++y) {
        auto& row = out.rows[static_cast<usize>(y - y0)];
        for (i32 x = x0; x < x1; ++x) {
            bool in = false;
            for (const auto& f : figures) {
                const usize n = f.pts.size();
                const usize segs = f.closed ? n : (n > 0 ? n - 1 : 0);
                if (n == 1 && seg_dist2({double(x), double(y)}, f.pts[0], f.pts[0]) < r2) in = true;
                for (usize i = 0; i < segs && !in; ++i) {
                    if (seg_dist2({double(x), double(y)}, f.pts[i], f.pts[(i + 1) % n]) < r2) in = true;
                }
                if (in) break;
            }
            if (!in) continue;
            if (!row.empty() && row.back().second == x) row.back().second = x + 1;
            else row.push_back({x, x + 1});
        }
    }
    return out;
}

/// The four cubic Béziers of an ellipse in a box, counter-clockwise from the
/// right-hand point.
void ellipse_beziers(double l, double t, double r, double b, std::vector<GdiManager::PathPoint>& out) {
    constexpr double k = 0.5522847498;
    const double cx = (l + r) / 2, cy = (t + b) / 2, rx = (r - l) / 2, ry = (b - t) / 2;
    const double ox = rx * k, oy = ry * k;
    out.push_back({r, cy, PT_MOVETO});
    const double pts[12][2] = {{r, cy - oy}, {cx + ox, t}, {cx, t},     {cx - ox, t},  {l, cy - oy}, {l, cy},
                               {l, cy + oy}, {cx - ox, b}, {cx, b},     {cx + ox, b},  {r, cy + oy}, {r, cy}};
    for (int i = 0; i < 12; ++i) out.push_back({pts[i][0], pts[i][1], PT_BEZIERTO});
    out.back().type |= PT_CLOSEFIGURE;
}

}  // namespace

// =============================================================================
// Transforms
// =============================================================================

bool GdiManager::dp_to_lp_d(const DcState& dc, double x, double y, double& ox, double& oy) const {
    ApiLock api_lock(*this);
    double wex = 1, wey = 1, vex = 1, vey = 1;
    switch (dc.map_mode) {
    case 2: wex = wey = 254; vex = 96; vey = -96; break;
    case 3: wex = wey = 2540; vex = 96; vey = -96; break;
    case 4: wex = wey = 100; vex = 96; vey = -96; break;
    case 5: wex = wey = 1000; vex = 96; vey = -96; break;
    case 6: wex = wey = 1440; vex = 96; vey = -96; break;
    case 7: case 8:
        wex = dc.window_ext_x;
        wey = dc.window_ext_y;
        vex = dc.viewport_ext_x;
        vey = dc.viewport_ext_y;
        break;
    default: break;
    }
    if (vex == 0 || vey == 0) return false;
    double wx = (x - dc.viewport_org_x) * wex / vex + dc.window_org_x;
    double wy = (y - dc.viewport_org_y) * wey / vey + dc.window_org_y;
    if (dc.graphics_mode == 2) {
        const WorldXform& w = dc.world_xform;
        const double det = static_cast<double>(w.eM11) * w.eM22 - static_cast<double>(w.eM12) * w.eM21;
        if (det == 0) return false;
        const double px = wx - w.eDx, py = wy - w.eDy;
        wx = (px * w.eM22 - py * w.eM21) / det;
        wy = (py * w.eM11 - px * w.eM12) / det;
    }
    ox = wx;
    oy = wy;
    return true;
}

bool GdiManager::dp_to_lp_points(Handle hdc, std::vector<IPoint>& pts) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    for (auto& p : pts) {
        double x = 0, y = 0;
        if (!dp_to_lp_d(it->second, p.first, p.second, x, y)) return false;
        p = {round_even(x), round_even(y)};
    }
    return true;
}

bool GdiManager::lp_to_dp_points(Handle hdc, std::vector<IPoint>& pts) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    for (auto& p : pts) {
        double x = 0, y = 0;
        lp_to_dp(it->second, p.first, p.second, x, y);
        p = {round_even(x), round_even(y)};
    }
    return true;
}

i32 GdiManager::set_map_mode(Handle hdc, i32 mode) {
    ApiLock api_lock(*this);
    // MM_TEXT (1) .. MM_ANISOTROPIC (8); anything else fails with the error
    // untouched ("setmapmode 0=0", "setmapmode 9=0").
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || mode < 1 || mode > 8) return 0;
    DcState& dc = it->second;
    const i32 prev = dc.map_mode;
    dc.map_mode = mode;
    if (mode == 1 || (mode >= 2 && mode <= 6)) {
        // Fixed modes reset the extents; the scaled modes keep them.
        dc.window_ext_x = dc.window_ext_y = dc.viewport_ext_x = dc.viewport_ext_y = 1;
    }
    return prev;
}

i32 GdiManager::get_map_mode(Handle hdc) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    return it == m_dc_states.end() ? 0 : it->second.map_mode;
}

namespace {
/// MM_ISOTROPIC keeps one logical unit square: the viewport axis that would
/// scale more is shrunk to the other's scale ("isotropic viewport ext -> 50,50"
/// for a 100x100 window and a 50x200 viewport).
void isotropic_adjust(i32 wx, i32 wy, i32& vx, i32& vy) {
    if (wx == 0 || wy == 0) return;
    const double sx = std::abs(static_cast<double>(vx) / wx), sy = std::abs(static_cast<double>(vy) / wy);
    if (sx < sy) vy = (vy < 0 ? -1 : 1) * static_cast<i32>(std::lround(std::abs(wy) * sx));
    else if (sy < sx) vx = (vx < 0 ? -1 : 1) * static_cast<i32>(std::lround(std::abs(wx) * sy));
}
}  // namespace

bool GdiManager::set_window_ext(Handle hdc, i32 x, i32 y, IPoint* old) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    DcState& dc = it->second;
    if (old) *old = {dc.window_ext_x, dc.window_ext_y};
    // Only the scaled modes take extents; the others ignore the call and
    // still succeed ("setviewportext in text=1").
    if (dc.map_mode != 7 && dc.map_mode != 8) return true;
    if (x == 0 || y == 0) return false;
    dc.window_ext_x = x;
    dc.window_ext_y = y;
    if (dc.map_mode == 7) isotropic_adjust(dc.window_ext_x, dc.window_ext_y, dc.viewport_ext_x, dc.viewport_ext_y);
    return true;
}

bool GdiManager::set_viewport_ext(Handle hdc, i32 x, i32 y, IPoint* old) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    DcState& dc = it->second;
    if (old) *old = {dc.viewport_ext_x, dc.viewport_ext_y};
    if (dc.map_mode != 7 && dc.map_mode != 8) return true;
    if (x == 0 || y == 0) return false;
    dc.viewport_ext_x = x;
    dc.viewport_ext_y = y;
    if (dc.map_mode == 7) isotropic_adjust(dc.window_ext_x, dc.window_ext_y, dc.viewport_ext_x, dc.viewport_ext_y);
    return true;
}

bool GdiManager::get_window_ext(Handle hdc, IPoint& out) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    out = {it->second.window_ext_x, it->second.window_ext_y};
    return true;
}

bool GdiManager::get_viewport_ext(Handle hdc, IPoint& out) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    out = {it->second.viewport_ext_x, it->second.viewport_ext_y};
    return true;
}

bool GdiManager::get_world_transform(Handle hdc, std::array<float, 6>& out) const {
    ApiLock api_lock(*this);
    // GM_COMPATIBLE DCs answer their (identity) transform too ("xf initial
    // get=1 1,0,0,1,0,0").
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    const WorldXform& w = it->second.world_xform;
    out = {w.eM11, w.eM12, w.eM21, w.eM22, w.eDx, w.eDy};
    return true;
}

bool GdiManager::set_world_transform(Handle hdc, const std::array<float, 6>& xf) {
    ApiLock api_lock(*this);
    // Only in GM_ADVANCED, and never singular ("setworld compat=0",
    // "setworld singular=0"), error untouched.
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.graphics_mode != 2) return false;
    if (static_cast<double>(xf[0]) * xf[3] - static_cast<double>(xf[1]) * xf[2] == 0) return false;
    it->second.world_xform = WorldXform{xf[0], xf[1], xf[2], xf[3], xf[4], xf[5]};
    return true;
}

bool GdiManager::modify_world_transform(Handle hdc, const std::array<float, 6>* xf, i32 mode) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.graphics_mode != 2) return false;
    WorldXform& cur = it->second.world_xform;
    if (mode == 1) {  // MWT_IDENTITY
        cur = WorldXform::identity();
        return true;
    }
    if ((mode != 2 && mode != 3) || xf == nullptr) return false;
    // Row-vector composition: LEFTMULTIPLY applies @p xf first (xf * cur),
    // RIGHTMULTIPLY after (cur * xf) ("xf left ... 25,67", "xf right ... 35,87").
    const WorldXform x{(*xf)[0], (*xf)[1], (*xf)[2], (*xf)[3], (*xf)[4], (*xf)[5]};
    const WorldXform& a = mode == 2 ? x : cur;
    const WorldXform& b = mode == 2 ? cur : x;
    WorldXform r;
    r.eM11 = a.eM11 * b.eM11 + a.eM12 * b.eM21;
    r.eM12 = a.eM11 * b.eM12 + a.eM12 * b.eM22;
    r.eM21 = a.eM21 * b.eM11 + a.eM22 * b.eM21;
    r.eM22 = a.eM21 * b.eM12 + a.eM22 * b.eM22;
    r.eDx = a.eDx * b.eM11 + a.eDy * b.eM21 + b.eDx;
    r.eDy = a.eDx * b.eM12 + a.eDy * b.eM22 + b.eDy;
    if (static_cast<double>(r.eM11) * r.eM22 - static_cast<double>(r.eM12) * r.eM21 == 0) return false;
    cur = r;
    return true;
}

i32 GdiManager::set_graphics_mode(Handle hdc, i32 mode) {
    ApiLock api_lock(*this);
    // GM_COMPATIBLE (1) / GM_ADVANCED (2); leaving GM_ADVANCED with a
    // transform set still succeeds ("setgraphicsmode compat with xform=2").
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || (mode != 1 && mode != 2)) return 0;
    const i32 prev = it->second.graphics_mode;
    it->second.graphics_mode = mode;
    return prev;
}

bool GdiManager::set_miter_limit(Handle hdc, float limit, float* old) {
    ApiLock api_lock(*this);
    // Below 1.0 fails, error untouched ("setmiterlimit 0.5=0").
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || !(limit >= 1.0f)) return false;
    if (old) *old = it->second.miter_limit;
    it->second.miter_limit = limit;
    return true;
}

bool GdiManager::get_miter_limit(Handle hdc, float& out) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    out = it->second.miter_limit;
    return true;
}

i32 GdiManager::set_arc_direction(Handle hdc, i32 dir) {
    ApiLock api_lock(*this);
    // AD_COUNTERCLOCKWISE (1) / AD_CLOCKWISE (2): the previous direction;
    // anything else 0 with the error untouched.
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || (dir != 1 && dir != 2)) return 0;
    const i32 prev = it->second.arc_direction;
    it->second.arc_direction = dir;
    return prev;
}

i32 GdiManager::get_arc_direction(Handle hdc) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    return it == m_dc_states.end() ? 0 : it->second.arc_direction;
}

// =============================================================================
// Device capabilities
// =============================================================================

i32 GdiManager::device_caps(Handle hdc, i32 index) const {
    ApiLock api_lock(*this);
    // Every DC kind answers the display's capabilities (s3gdi_info_probe:
    // screen, memory and monochrome memory DCs agree); a bad DC or index is 0
    // with the error untouched ("caps NULL dc=0", "caps 999=0").
    if (!m_dc_states.contains(hdc)) return 0;
    i32 w = 1920, h = 1080, refresh = 60;
    if (m_virtual_screen_width > 0 && m_virtual_screen_height > 0) {
        w = m_virtual_screen_width;
        h = m_virtual_screen_height;
    } else if (m_cocoa_bridge) {
        const auto bridge = m_cocoa_bridge;
        const auto mi = [&] {
            ApiUnlock unlocked(*this);
            return bridge->get_primary_monitor_info();
        }();
        w = mi.width;
        h = mi.height;
    }
    switch (index) {
    case 4: return (w * 254 + 480) / 960;   // HORZSIZE: mm at 96 dpi (1024 -> 271)
    case 6: return (h * 254 + 480) / 960;   // VERTSIZE (768 -> 203)
    case 8: case 118: return w;             // HORZRES, DESKTOPHORZRES
    case 10: case 117: return h;            // VERTRES, DESKTOPVERTRES
    case 116: return refresh;               // VREFRESH
    default: break;
    }
    for (const auto& [id, value] : gdi::kGdiDisplayCaps) {
        if (id == index) return value;
    }
    return 0;
}

i32 GdiManager::get_device_caps(Handle hdc, i32 index) {
    ApiLock api_lock(*this); return device_caps(hdc, index); }

// =============================================================================
// Regions from shapes
// =============================================================================

bool GdiManager::set_rect_region(Handle hrgn, i32 l, i32 t, i32 r, i32 b) {
    ApiLock api_lock(*this);
    // SetRectRgn orders the corners ("after setrectrgn inverted ... 10,5,20,30")
    // and an empty rectangle is the empty region.
    const auto it = m_region_objects.find(hrgn);
    if (it == m_region_objects.end()) return false;
    ClipRect rgn;
    rgn.left = std::min(l, r);
    rgn.right = std::max(l, r);
    rgn.top = std::min(t, b);
    rgn.bottom = std::max(t, b);
    if (rgn.left == rgn.right || rgn.top == rgn.bottom) rgn.left = rgn.top = rgn.right = rgn.bottom = 0;
    rgn.valid = true;
    it->second = rgn;
    return true;
}

Handle GdiManager::create_polygon_region(const std::vector<IPoint>& pts, const std::vector<i32>& counts,
                                         i32 fill_mode) {
    ApiLock api_lock(*this);
    std::vector<Poly> polys;
    usize at = 0;
    for (const i32 n : counts) {
        if (n < 0 || at + static_cast<usize>(n) > pts.size()) return 0;
        Poly p;
        for (i32 i = 0; i < n; ++i, ++at) p.push_back({double(pts[at].first), double(pts[at].second)});
        polys.push_back(std::move(p));
    }
    const Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    m_region_objects[h] = coverage_region(polygon_coverage(polys, fill_mode));
    return h;
}

Handle GdiManager::create_elliptic_region(i32 l, i32 t, i32 r, i32 b) {
    ApiLock api_lock(*this);
    // Windows' elliptic regions sit one pixel inside the box (0,0,10,10 ->
    // bounds 0,0,9,9; 0,0,7,5 -> 1,0,5,4). The ellipse through the pixel
    // centres of (l, t)..(r-2, b-2) gives those bounds; the per-row spans of
    // Windows' own integer algorithm can differ by a pixel.
    std::vector<PathPoint> pts;
    const double L = std::min(l, r), T = std::min(t, b), R = std::max(l, r), B = std::max(t, b);
    ellipse_beziers(L - 0.5, T - 0.5, R - 1.5, B - 1.5, pts);
    std::vector<Poly> polys;
    for (auto& f : figures_of(pts)) polys.push_back(std::move(f.pts));
    const Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    m_region_objects[h] = coverage_region(polygon_coverage(polys, 1));
    return h;
}

Handle GdiManager::create_round_rect_region(i32 l, i32 t, i32 r, i32 b, i32 w, i32 h) {
    ApiLock api_lock(*this);
    // Like elliptic regions, one pixel inside the box ("roundrect
    // 0,0,20,12,8,8" -> bounds 0,0,19,11).
    const double L = std::min(l, r) - 0.5, T = std::min(t, b) - 0.5, R = std::max(l, r) - 1.5,
                 B = std::max(t, b) - 1.5;
    const double rx = std::min(std::abs(w) / 2.0, (R - L) / 2), ry = std::min(std::abs(h) / 2.0, (B - T) / 2);
    Poly p;
    auto arc = [&](double cx, double cy, double a0) {
        for (int i = 0; i <= 8; ++i) {
            const double a = a0 + i * (M_PI / 2) / 8;
            p.push_back({cx + rx * std::cos(a), cy - ry * std::sin(a)});
        }
    };
    arc(R - rx, T + ry, 0);
    arc(L + rx, T + ry, M_PI / 2);
    arc(L + rx, B - ry, M_PI);
    arc(R - rx, B - ry, 3 * M_PI / 2);
    const Handle hr = allocate_gdi_handle(kHandleTypeGdiObject);
    m_region_objects[hr] = coverage_region(polygon_coverage({p}, 1));
    return hr;
}

i32 GdiManager::get_clip_region(Handle hdc, Handle hrgn) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    const auto rgn = m_region_objects.find(hrgn);
    if (it == m_dc_states.end() || rgn == m_region_objects.end()) return -1;
    if (!it->second.clip_rect.valid) return 0;
    rgn->second = it->second.clip_rect;
    return 1;
}

// =============================================================================
// Paths
// =============================================================================

bool GdiManager::begin_path(Handle hdc) {
    ApiLock api_lock(*this);
    // A new bracket discards any previous path ("beginpath again discards=1").
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    it->second.path = PathState{};
    it->second.path.state = 1;
    return true;
}

bool GdiManager::end_path(Handle hdc) {
    ApiLock api_lock(*this);
    // Without an open bracket: FALSE, error untouched ("endpath nopath=0").
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 1) return false;
    it->second.path.state = 2;
    return true;
}

bool GdiManager::abort_path(Handle hdc) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    it->second.path = PathState{};
    return true;
}

bool GdiManager::close_figure(Handle hdc) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 1) return false;
    PathState& p = it->second.path;
    if (!p.points.empty() && p.figure_open) {
        p.points.back().type |= PT_CLOSEFIGURE;
        p.figure_open = false;
    }
    return true;
}

bool GdiManager::path_record_move(Handle hdc, i32 x, i32 y) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 1) return false;
    DcState& dc = it->second;
    dc.current_x = x;
    dc.current_y = y;
    double dx = 0, dy = 0;
    lp_to_dp(dc, x, y, dx, dy);
    // Consecutive moves replace each other.
    if (!dc.path.points.empty() && (dc.path.points.back().type & ~PT_CLOSEFIGURE) == PT_MOVETO &&
        !(dc.path.points.back().type & PT_CLOSEFIGURE)) {
        dc.path.points.back() = {dx, dy, PT_MOVETO};
    } else {
        dc.path.points.push_back({dx, dy, PT_MOVETO});
    }
    dc.path.figure_open = true;
    return true;
}

void GdiManager::path_ensure_figure(DcState& dc, double cx, double cy) {
    // Start a figure at the current position when the last one was closed.
    if (!dc.path.figure_open || dc.path.points.empty()) {
        dc.path.points.push_back({cx, cy, PT_MOVETO});
        dc.path.figure_open = true;
    }
}

bool GdiManager::path_record_line(Handle hdc, i32 x, i32 y) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 1) return false;
    DcState& dc = it->second;
    double cx = 0, cy = 0, dx = 0, dy = 0;
    lp_to_dp(dc, dc.current_x, dc.current_y, cx, cy);
    lp_to_dp(dc, x, y, dx, dy);
    path_ensure_figure(dc, cx, cy);
    dc.path.points.push_back({dx, dy, PT_LINETO});
    dc.current_x = x;
    dc.current_y = y;
    return true;
}

bool GdiManager::path_record_rect(Handle hdc, i32 l, i32 t, i32 r, i32 b) {
    ApiLock api_lock(*this);
    // The rectangle's own corners, a closed figure ("strokeandfill rect":
    // Rectangle(4,4,20,16) covers x 4..20, y 4..16 once stroked).
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 1) return false;
    DcState& dc = it->second;
    const i32 xs[4] = {l, r, r, l}, ys[4] = {t, t, b, b};
    for (int i = 0; i < 4; ++i) {
        double dx = 0, dy = 0;
        lp_to_dp(dc, xs[i], ys[i], dx, dy);
        dc.path.points.push_back({dx, dy, i == 0 ? PT_MOVETO : PT_LINETO});
    }
    dc.path.points.back().type |= PT_CLOSEFIGURE;
    dc.path.figure_open = false;
    return true;
}

bool GdiManager::path_record_ellipse(Handle hdc, i32 l, i32 t, i32 r, i32 b) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 1) return false;
    DcState& dc = it->second;
    std::vector<PathPoint> pts;
    ellipse_beziers(l, t, r, b, pts);
    for (auto& p : pts) {
        double dx = 0, dy = 0;
        lp_to_dp(dc, p.x, p.y, dx, dy);
        dc.path.points.push_back({dx, dy, p.type});
    }
    dc.path.figure_open = false;
    return true;
}

bool GdiManager::path_record_round_rect(Handle hdc, i32 l, i32 t, i32 r, i32 b, i32 w, i32 h) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 1) return false;
    DcState& dc = it->second;
    const double rx = std::min(std::abs(w) / 2.0, std::abs(r - l) / 2.0);
    const double ry = std::min(std::abs(h) / 2.0, std::abs(b - t) / 2.0);
    constexpr double k = 0.5522847498;
    const double L = l, T = t, R = r, B = b;
    const double pts[][3] = {
        {R, T + ry, PT_MOVETO},
        {R, T + ry - ry * k, PT_BEZIERTO}, {R - rx + rx * k, T, PT_BEZIERTO}, {R - rx, T, PT_BEZIERTO},
        {L + rx, T, PT_LINETO},
        {L + rx - rx * k, T, PT_BEZIERTO}, {L, T + ry - ry * k, PT_BEZIERTO}, {L, T + ry, PT_BEZIERTO},
        {L, B - ry, PT_LINETO},
        {L, B - ry + ry * k, PT_BEZIERTO}, {L + rx - rx * k, B, PT_BEZIERTO}, {L + rx, B, PT_BEZIERTO},
        {R - rx, B, PT_LINETO},
        {R - rx + rx * k, B, PT_BEZIERTO}, {R, B - ry + ry * k, PT_BEZIERTO}, {R, B - ry, PT_BEZIERTO},
    };
    for (const auto& p : pts) {
        double dx = 0, dy = 0;
        lp_to_dp(dc, p[0], p[1], dx, dy);
        dc.path.points.push_back({dx, dy, static_cast<u8>(p[2])});
    }
    dc.path.points.back().type |= PT_CLOSEFIGURE;
    dc.path.figure_open = false;
    return true;
}

bool GdiManager::fill_path(Handle hdc) {
    ApiLock api_lock(*this);
    // FillPath needs a closed bracket ("fillpath open=0", "fillpath nopath=0",
    // error untouched), consumes the path ("fillpath again=0") and fills
    // nothing for an empty one ("fillpath empty=1").
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 2) return false;
    DcState& dc = it->second;
    std::vector<Poly> polys;
    for (auto& f : figures_of(dc.path.points)) polys.push_back(std::move(f.pts));
    dc.path = PathState{};
    const Coverage c = polygon_coverage(polys, dc.poly_fill_mode);
    RasterTarget t;
    if (!raster_target(hdc, t)) return true;
    bool any = false;
    for (usize r = 0; r < c.rows.size(); ++r) {
        const i32 y = c.y0 + static_cast<i32>(r);
        for (const auto& [x0, x1] : c.rows[r]) {
            for (i32 x = x0; x < x1; ++x) {
                if (!raster_writable(t, x, y)) continue;
                u32 p = 0;
                if (!brush_pattern_word(t, dc.selected_brush, x, y, p)) return true;
                u32 d = 0;
                std::memcpy(&d, t.surface->pixel_at(x, y), 4);
                raster_store(t, x, y, rop2_mix(dc.rop2, p, d));
                any = true;
            }
        }
    }
    if (any) finish_raster(t);
    return true;
}

bool GdiManager::stroke_path(Handle hdc) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 2) return false;
    DcState& dc = it->second;
    const auto figures = figures_of(dc.path.points);
    dc.path = PathState{};
    RasterTarget t;
    if (!raster_target(hdc, t)) return true;
    u32 pen = 0;
    i32 width = 1;
    if (!pen_word(t, pen, width)) return true;  // PS_NULL
    for (const auto& f : figures) {
        const usize n = f.pts.size();
        if (n == 0) continue;
        std::vector<IPoint> ip;
        for (const auto& p : f.pts) ip.push_back({round_even(p.x), round_even(p.y)});
        for (usize i = 0; i + 1 < n; ++i) {
            draw_line_pixels(t, ip[i].first, ip[i].second, ip[i + 1].first, ip[i + 1].second, pen, width);
        }
        if (f.closed && n > 1) {
            draw_line_pixels(t, ip[n - 1].first, ip[n - 1].second, ip[0].first, ip[0].second, pen, width);
        } else {
            plot_pen(t, ip[n - 1].first, ip[n - 1].second, pen, width);
        }
    }
    finish_raster(t);
    return true;
}

bool GdiManager::stroke_and_fill_path(Handle hdc) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 2) return false;
    const PathState saved = it->second.path;
    if (!fill_path(hdc)) return false;
    it->second.path = saved;
    return stroke_path(hdc);
}

bool GdiManager::flatten_path(Handle hdc) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 2) return false;
    PathState& p = it->second.path;
    std::vector<PathPoint> flat;
    for (const auto& f : figures_of(p.points)) {
        for (usize i = 0; i < f.pts.size(); ++i) {
            flat.push_back({f.pts[i].x, f.pts[i].y, i == 0 ? PT_MOVETO : PT_LINETO});
        }
        if (f.closed && !flat.empty()) flat.back().type |= PT_CLOSEFIGURE;
    }
    p.points = std::move(flat);
    return true;
}

bool GdiManager::widen_path(Handle hdc) {
    ApiLock api_lock(*this);
    // The area the pen would paint, as disjoint one-row rectangles: filling
    // or clipping to it matches Windows; its points (GetPath) do not.
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 2) return false;
    DcState& dc = it->second;
    i32 width = 1;
    if (const auto pen = m_pen_objects.find(dc.selected_pen); pen != m_pen_objects.end()) {
        if ((pen->second.style & 0x0F) == 5) return false;  // PS_NULL
        width = std::max(1, pen->second.width);
    } else if (const auto ext = m_extpen_objects.find(dc.selected_pen); ext != m_extpen_objects.end()) {
        width = std::max<i32>(1, static_cast<i32>(ext->second.width));
    }
    const Coverage c = widened_coverage(figures_of(dc.path.points), width / 2.0);
    std::vector<PathPoint> out;
    for (usize r = 0; r < c.rows.size(); ++r) {
        const double y = c.y0 + static_cast<double>(r);
        for (const auto& [x0, x1] : c.rows[r]) {
            out.push_back({double(x0), y, PT_MOVETO});
            out.push_back({double(x1), y, PT_LINETO});
            out.push_back({double(x1), y + 1, PT_LINETO});
            out.push_back({double(x0), y + 1, static_cast<u8>(PT_LINETO | PT_CLOSEFIGURE)});
        }
    }
    dc.path.points = std::move(out);
    return true;
}

bool GdiManager::select_clip_path(Handle hdc, i32 mode) {
    ApiLock api_lock(*this);
    // The closed path becomes (or combines into) the clip region and is
    // consumed ("clip path clipbox=2 10,10,40,30").
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 2 || mode < 1 || mode > 5) return false;
    DcState& dc = it->second;
    std::vector<Poly> polys;
    for (auto& f : figures_of(dc.path.points)) polys.push_back(std::move(f.pts));
    dc.path = PathState{};
    const ClipRect region = coverage_region(polygon_coverage(polys, dc.poly_fill_mode));
    if (mode == 5 || !dc.clip_rect.valid) {
        if (mode == 5 || mode == 1 || mode == 2 || mode == 3) {
            if (!dc.clip_rect.valid && mode != 5) {
                ClipRect whole;
                whole.left = whole.top = 0;
                whole.right = dc.bitmap ? dc.bitmap->width : 32767;
                whole.bottom = dc.bitmap ? dc.bitmap->height : 32767;
                whole.valid = true;
                dc.clip_rect = ClipRect::combine(whole, region, mode);
            } else {
                dc.clip_rect = region;
            }
            return true;
        }
    }
    if (!dc.clip_rect.valid) {
        ClipRect whole;
        whole.right = dc.bitmap ? dc.bitmap->width : 32767;
        whole.bottom = dc.bitmap ? dc.bitmap->height : 32767;
        whole.valid = true;
        dc.clip_rect = whole;
    }
    dc.clip_rect = ClipRect::combine(dc.clip_rect, region, mode);
    return true;
}

Handle GdiManager::path_to_region(Handle hdc) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 2) return 0;
    DcState& dc = it->second;
    std::vector<Poly> polys;
    for (auto& f : figures_of(dc.path.points)) polys.push_back(std::move(f.pts));
    dc.path = PathState{};
    const Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    m_region_objects[h] = coverage_region(polygon_coverage(polys, dc.poly_fill_mode));
    return h;
}

i32 GdiManager::get_path(Handle hdc, std::vector<IPoint>* pts, std::vector<u8>* types) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || it->second.path.state != 2) return -1;
    const DcState& dc = it->second;
    for (const auto& p : dc.path.points) {
        double lx = 0, ly = 0;
        if (!dp_to_lp_d(dc, p.x, p.y, lx, ly)) return -1;
        if (pts) pts->push_back({round_even(lx), round_even(ly)});
        if (types) types->push_back(p.type);
    }
    return static_cast<i32>(dc.path.points.size());
}

// =============================================================================
// Polylines, polygons, Béziers
// =============================================================================

bool GdiManager::polyline(Handle hdc, const std::vector<IPoint>& pts) {
    ApiLock api_lock(*this);
    // Fewer than two points: FALSE, error untouched ("polyline 1pt=0").
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || pts.size() < 2) return false;
    DcState& dc = it->second;
    if (dc.path.state == 1) {
        const i32 cx = dc.current_x, cy = dc.current_y;
        (void)path_record_move(hdc, pts[0].first, pts[0].second);
        for (usize i = 1; i < pts.size(); ++i) (void)path_record_line(hdc, pts[i].first, pts[i].second);
        dc.current_x = cx;  // Polyline leaves the current position alone
        dc.current_y = cy;
        return true;
    }
    RasterTarget t;
    if (!raster_target(hdc, t)) return true;
    u32 pen = 0;
    i32 width = 1;
    if (!pen_word(t, pen, width)) return true;
    std::vector<IPoint> d = pts;
    for (auto& p : d) lp_to_dp_point(dc, p.first, p.second);
    for (usize i = 0; i + 1 < d.size(); ++i) {
        draw_line_pixels(t, d[i].first, d[i].second, d[i + 1].first, d[i + 1].second, pen, width);
    }
    finish_raster(t);
    return true;
}

bool GdiManager::polyline_to(Handle hdc, const std::vector<IPoint>& pts) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || pts.empty()) return false;
    DcState& dc = it->second;
    if (dc.path.state == 1) {
        for (const auto& p : pts) (void)path_record_line(hdc, p.first, p.second);
        return true;
    }
    std::vector<IPoint> all{{dc.current_x, dc.current_y}};
    all.insert(all.end(), pts.begin(), pts.end());
    const bool ok = polyline(hdc, all);
    dc.current_x = pts.back().first;
    dc.current_y = pts.back().second;
    return ok;
}

bool GdiManager::poly_polygon(Handle hdc, const std::vector<IPoint>& pts, const std::vector<i32>& counts) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || counts.empty()) return false;
    for (const i32 n : counts) {
        if (n < 2) return false;  // "polygon 1pt=0"
    }
    DcState& dc = it->second;
    usize at = 0;
    if (dc.path.state == 1) {
        for (const i32 n : counts) {
            for (i32 i = 0; i < n; ++i, ++at) {
                double dx = 0, dy = 0;
                lp_to_dp(dc, pts[at].first, pts[at].second, dx, dy);
                dc.path.points.push_back({dx, dy, i == 0 ? PT_MOVETO : PT_LINETO});
            }
            dc.path.points.back().type |= PT_CLOSEFIGURE;
        }
        dc.path.figure_open = false;
        return true;
    }
    std::vector<Poly> polys;
    std::vector<std::vector<IPoint>> device;
    for (const i32 n : counts) {
        Poly p;
        std::vector<IPoint> d;
        for (i32 i = 0; i < n; ++i, ++at) {
            i32 x = pts[at].first, y = pts[at].second;
            lp_to_dp_point(dc, x, y);
            p.push_back({double(x), double(y)});
            d.push_back({x, y});
        }
        polys.push_back(std::move(p));
        device.push_back(std::move(d));
    }
    RasterTarget t;
    if (!raster_target(hdc, t)) return true;
    // Interior with the brush, then the outline with the pen ("polygon tri":
    // 210 interior pixels + the border = 231).
    const Coverage c = polygon_coverage(polys, dc.poly_fill_mode);
    for (usize r = 0; r < c.rows.size(); ++r) {
        const i32 y = c.y0 + static_cast<i32>(r);
        for (const auto& [x0, x1] : c.rows[r]) {
            for (i32 x = x0; x < x1; ++x) {
                if (!raster_writable(t, x, y)) continue;
                u32 p = 0;
                if (!brush_pattern_word(t, dc.selected_brush, x, y, p)) break;
                u32 d = 0;
                std::memcpy(&d, t.surface->pixel_at(x, y), 4);
                raster_store(t, x, y, rop2_mix(dc.rop2, p, d));
            }
        }
    }
    u32 pen = 0;
    i32 width = 1;
    if (pen_word(t, pen, width)) {
        for (const auto& d : device) {
            for (usize i = 0; i < d.size(); ++i) {
                const auto& a = d[i];
                const auto& b = d[(i + 1) % d.size()];
                draw_line_pixels(t, a.first, a.second, b.first, b.second, pen, width);
            }
        }
    }
    finish_raster(t);
    return true;
}

bool GdiManager::poly_bezier(Handle hdc, const std::vector<IPoint>& pts, bool to) {
    ApiLock api_lock(*this);
    // PolyBezier takes 1 + 3n points, PolyBezierTo 3n.
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    DcState& dc = it->second;
    if (to ? (pts.empty() || pts.size() % 3 != 0) : (pts.size() < 4 || (pts.size() - 1) % 3 != 0)) return false;
    std::vector<IPoint> all;
    if (to) all.push_back({dc.current_x, dc.current_y});
    all.insert(all.end(), pts.begin(), pts.end());
    if (dc.path.state == 1) {
        if (!to) (void)path_record_move(hdc, all[0].first, all[0].second);
        double cx = 0, cy = 0;
        lp_to_dp(dc, all[0].first, all[0].second, cx, cy);
        path_ensure_figure(dc, cx, cy);
        for (usize i = 1; i < all.size(); ++i) {
            double dx = 0, dy = 0;
            lp_to_dp(dc, all[i].first, all[i].second, dx, dy);
            dc.path.points.push_back({dx, dy, PT_BEZIERTO});
        }
        if (to) {
            dc.current_x = all.back().first;
            dc.current_y = all.back().second;
        }
        return true;
    }
    Poly flat;
    double sx = 0, sy = 0;
    lp_to_dp(dc, all[0].first, all[0].second, sx, sy);
    flat.push_back({sx, sy});
    for (usize i = 1; i + 2 < all.size(); i += 3) {
        Pt p[3];
        for (int k = 0; k < 3; ++k) lp_to_dp(dc, all[i + k].first, all[i + k].second, p[k].x, p[k].y);
        flatten_bezier(flat.back(), p[0], p[1], p[2], flat, 0);
    }
    if (to) {
        dc.current_x = all.back().first;
        dc.current_y = all.back().second;
    }
    RasterTarget t;
    if (!raster_target(hdc, t)) return true;
    u32 pen = 0;
    i32 width = 1;
    if (!pen_word(t, pen, width)) return true;
    for (usize i = 0; i + 1 < flat.size(); ++i) {
        draw_line_pixels(t, round_even(flat[i].x), round_even(flat[i].y), round_even(flat[i + 1].x),
                         round_even(flat[i + 1].y), pen, width);
    }
    finish_raster(t);
    return true;
}

}  // namespace vitreo::winapi
