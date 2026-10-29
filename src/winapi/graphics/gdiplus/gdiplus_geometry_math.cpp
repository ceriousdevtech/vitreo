// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_geometry_math.cpp
/// @brief GraphicsPath geometry shared by the GDI+ areas: the point arrays
/// each GdipAddPath* builder emits, flattening, bounds, reversal, winding
/// outline, warping, point containment and the CoreGraphics bridge.
///
/// Every emitted point array was measured on Windows 11 with
/// tools/oracle_probe/gdiplus_geometry_probe.c (.x64.expected /
/// .x86.expected). Probe case names are quoted beside the rule they fix.
/// Notes and the remaining divergences: .docs/contracts/gdiplus/geometry.md.

#include "winapi/graphics/gdiplus/gdiplus_geometry.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace vitreo::winapi::gdip {

namespace {

constexpr double kPi = 3.14159265358979323846;
/// 4/3 (sqrt 2 - 1): the quarter-ellipse Bézier constant GdipAddPathEllipse
/// uses ("ellipse(10,20,100,50)": 45 + 25 * 0.5522847 = 58.8071175).
constexpr float kEllipseK = 0.5522847498f;

[[nodiscard]] bool same_point(PointF a, PointF b) { return a.X == b.X && a.Y == b.Y; }

/// Append the first point of a primitive that joins the open figure: a
/// Start when a new figure begins, else a Line, skipped when it repeats the
/// figure's last point ("lines_connect": AddLine(30,30,40,0) after a line
/// ending at 30,30 adds only 40,0; "line_after_degenerate").
void connect_first(GpPath& path, PointF p) {
    if (path.new_figure || path.points.empty()) {
        path.points.push_back(p);
        path.types.push_back(PathPointTypeStart);
        path.new_figure = false;
        return;
    }
    if (same_point(path.points.back(), p)) return;
    path.points.push_back(p);
    path.types.push_back(PathPointTypeLine);
}

void push(GpPath& path, PointF p, u8 type) {
    path.points.push_back(p);
    path.types.push_back(type);
}

/// A primitive that is always its own closed figure (rectangle, ellipse,
/// polygon, closed curve): the next primitive starts a new figure.
void close_last(GpPath& path) {
    if (path.points.empty()) return;
    path.types.back() |= PathPointTypeCloseSubpath;
    path.new_figure = true;
}

/// Visual angle (degrees, measured to the point on the ellipse as GDI+
/// does) -> parametric angle (radians), on the same turn as @p deg.
/// "arc(10,20,100,50,30,120)": 30 deg on a 2:1 ellipse is t = 49.1 deg.
double param_angle(double deg, double rx, double ry) {
    const double rad = deg * kPi / 180.0;
    double t = std::atan2(rx * std::sin(rad), ry * std::cos(rad));
    t += 2.0 * kPi * std::round((rad - t) / (2.0 * kPi));
    return t;
}

struct PointD {
    double x{0}, y{0};
};

[[nodiscard]] PointD bez_at(const PointD* p, double t) {
    const double u = 1.0 - t;
    const double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
    return {a * p[0].x + b * p[1].x + c * p[2].x + d * p[3].x,
            a * p[0].y + b * p[1].y + c * p[2].y + d * p[3].y};
}

/// Flatten one Bézier the way GdipFlattenPath does: control points on a
/// grid of flatness/4, hybrid forward differencing with steps of 2^-k
/// chosen so that h^2 * |(1-t)A + tB| <= flatness at both ends of every step
/// (A, B the control polygon's second differences), the step doubled again
/// as soon as an aligned double step meets the same bound. Emitted points
/// are on the same grid. Fitted to every probe count ("flatten_ellipse"
/// 0.1 / 0.25 / 1 / 5 / 100, "flatten_bez", "flatten_big_bez",
/// "flatten_neg_bez", "flatten_tiny_bez", "flatten_ellipse_m").
void flatten_bezier(const PointF* in, float flatness, std::vector<PointF>& out) {
    const double scale = 4.0 / static_cast<double>(flatness);
    const double tol = std::fabs(static_cast<double>(flatness));
    // GDI+ converts with (int)(v + 0.5): truncation toward zero, which the
    // negative-flatness case ("flatten_ellipse(-1)") exposes.
    auto snap = [scale](double v) {
        return static_cast<double>(static_cast<i64>(v * scale + 0.5)) / scale;
    };
    PointD p[4];
    for (int i = 0; i < 4; ++i) p[i] = {snap(in[i].X), snap(in[i].Y)};
    const double ax = p[2].x - 2 * p[1].x + p[0].x, ay = p[2].y - 2 * p[1].y + p[0].y;
    const double bx = p[3].x - 2 * p[2].x + p[1].x, by = p[3].y - 2 * p[2].y + p[1].y;
    auto coef = [&](double t) {
        return std::max(std::fabs(ax * (1 - t) + bx * t), std::fabs(ay * (1 - t) + by * t));
    };
    constexpr int kBits = 30;
    constexpr u64 kEnd = u64{1} << kBits;
    constexpr int kMaxDepth = 20;
    auto error = [&](u64 pos, int k) {
        const double t = static_cast<double>(pos) / static_cast<double>(kEnd);
        const double h = std::ldexp(1.0, -k);
        return std::max(coef(t), coef(std::min(1.0, t + h))) * h * h;
    };
    u64 pos = 0;
    int k = 0;
    while (pos < kEnd) {
        while (k < kMaxDepth && error(pos, k) > tol) ++k;
        pos += u64{1} << (kBits - k);
        if (pos >= kEnd) {
            out.push_back({static_cast<float>(p[3].x), static_cast<float>(p[3].y)});
            break;
        }
        const PointD q = bez_at(p, static_cast<double>(pos) / static_cast<double>(kEnd));
        out.push_back({static_cast<float>(snap(q.x)), static_cast<float>(snap(q.y))});
        while (k > 0 && (pos & ((u64{1} << (kBits - k + 1)) - 1)) == 0 &&
               error(pos, k - 1) <= tol) {
            --k;
        }
    }
}

[[nodiscard]] float snap_flat(float v, float flatness) {
    const double scale = 4.0 / static_cast<double>(flatness);
    return static_cast<float>(static_cast<double>(static_cast<i64>(v * scale + 0.5)) / scale);
}

/// Index ranges [start, end] of each figure.
std::vector<std::pair<usize, usize>> figures(const GpPath& path) {
    std::vector<std::pair<usize, usize>> out;
    const usize n = path.points.size();
    usize i = 0;
    while (i < n) {
        usize j = i + 1;
        while (j < n && (path.types[j] & PathPointTypePathTypeMask) != PathPointTypeStart) ++j;
        out.emplace_back(i, j - 1);
        i = j;
    }
    return out;
}

/// Nonzero/alternate winding of @p p against the closed polygons @p polys.
int winding_number(const std::vector<std::vector<PointD>>& polys, PointD p) {
    int w = 0;
    for (const auto& poly : polys) {
        const usize n = poly.size();
        if (n < 2) continue;
        for (usize i = 0; i < n; ++i) {
            const PointD a = poly[i], b = poly[(i + 1) % n];
            if (a.y <= p.y) {
                if (b.y > p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) > 0) ++w;
            } else if (b.y <= p.y &&
                       (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) < 0) {
                --w;
            }
        }
    }
    return w;
}

std::vector<std::vector<PointD>> polygons(const GpPath& flat) {
    std::vector<std::vector<PointD>> polys;
    for (auto [s, e] : figures(flat)) {
        std::vector<PointD> poly;
        for (usize i = s; i <= e; ++i) poly.push_back({flat.points[i].X, flat.points[i].Y});
        polys.push_back(std::move(poly));
    }
    return polys;
}

}  // namespace

i32 gp_round(float v) { return static_cast<i32>(std::floor(v + 0.5f)); }

// ---- Builders --------------------------------------------------------------

void path_add_lines(GpPath& path, const std::vector<PointF>& pts) {
    if (pts.empty()) return;
    connect_first(path, pts[0]);
    // Repeats inside the array are kept ("line2_dups").
    for (usize i = 1; i < pts.size(); ++i) push(path, pts[i], PathPointTypeLine);
}

void path_add_beziers(GpPath& path, const std::vector<PointF>& pts) {
    if (pts.size() < 4) return;
    connect_first(path, pts[0]);
    for (usize i = 1; i < pts.size(); ++i) push(path, pts[i], PathPointTypeBezier);
}

void path_add_arc(GpPath& path, RectF rect, float start, float sweep) {
    // "arc(10,20,100,50,0,0)": a zero sweep adds nothing and keeps the
    // figure open ("arc_zero_mid").
    if (sweep == 0) return;
    const bool full = std::fabs(sweep) >= 360.0f;  // "arc(..,10,400)" clamps
    const double rx = rect.Width / 2.0, ry = rect.Height / 2.0;
    const double cx = rect.X + rx, cy = rect.Y + ry;
    const double a = param_angle(start, rx, ry);
    double b;
    if (full) {
        b = a + (sweep > 0 ? 2.0 * kPi : -2.0 * kPi);
    } else {
        b = param_angle(static_cast<double>(start) + sweep, rx, ry);
    }
    const double total = b - a;
    // Segments of a quarter turn (parametric) from the start, the remainder
    // last ("arc(10,20,100,50,200,170)": 90 deg then 73.4 deg).
    const double quarter = kPi / 2.0;
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(total) / quarter - 1e-9)));
    const double dir = total < 0 ? -1.0 : 1.0;
    auto at = [&](double t) { return PointD{cx + rx * std::cos(t), cy + ry * std::sin(t)}; };
    connect_first(path, {static_cast<float>(at(a).x), static_cast<float>(at(a).y)});
    double t0 = a;
    for (int i = 0; i < n; ++i) {
        const double t1 = (i == n - 1) ? b : t0 + dir * quarter;
        const double k = 4.0 / 3.0 * std::tan((t1 - t0) / 4.0);
        const PointD p0 = at(t0), p3 = at(t1);
        const PointD p1{p0.x - k * rx * std::sin(t0), p0.y + k * ry * std::cos(t0)};
        const PointD p2{p3.x + k * rx * std::sin(t1), p3.y - k * ry * std::cos(t1)};
        push(path, {static_cast<float>(p1.x), static_cast<float>(p1.y)}, PathPointTypeBezier);
        push(path, {static_cast<float>(p2.x), static_cast<float>(p2.y)}, PathPointTypeBezier);
        push(path, {static_cast<float>(p3.x), static_cast<float>(p3.y)}, PathPointTypeBezier);
        t0 = t1;
    }
    // A full turn closes the figure it joined ("arc_full_then_line").
    if (full) close_last(path);
}

namespace {
/// Cardinal-spline tangents (GDI+: (P[i+1] - P[i-1]) * tension / 3, the
/// end points one-sided, "curve3"); @p closed wraps around ("closed3").
std::vector<PointF> curve_tangents(const std::vector<PointF>& p, float tension, bool closed) {
    const usize n = p.size();
    const float f = tension / 3.0f;
    std::vector<PointF> t(n);
    for (usize i = 0; i < n; ++i) {
        PointF prev, next;
        if (closed) {
            prev = p[(i + n - 1) % n];
            next = p[(i + 1) % n];
        } else {
            prev = p[i == 0 ? 0 : i - 1];
            next = p[i + 1 < n ? i + 1 : n - 1];
        }
        t[i] = {(next.X - prev.X) * f, (next.Y - prev.Y) * f};
    }
    return t;
}
}  // namespace

void path_add_curve(GpPath& path, const std::vector<PointF>& pts, i32 offset, i32 segments,
                    float tension) {
    const i32 n = static_cast<i32>(pts.size());
    if (n < 2 || offset < 0 || segments < 1 || offset + segments > n - 1) return;
    const std::vector<PointF> tan = curve_tangents(pts, tension, false);
    connect_first(path, pts[static_cast<usize>(offset)]);
    for (i32 s = offset; s < offset + segments; ++s) {
        const PointF a = pts[static_cast<usize>(s)], b = pts[static_cast<usize>(s + 1)];
        const PointF ta = tan[static_cast<usize>(s)], tb = tan[static_cast<usize>(s + 1)];
        push(path, {a.X + ta.X, a.Y + ta.Y}, PathPointTypeBezier);
        push(path, {b.X - tb.X, b.Y - tb.Y}, PathPointTypeBezier);
        push(path, b, PathPointTypeBezier);
    }
}

void path_add_closed_curve(GpPath& path, const std::vector<PointF>& pts, float tension) {
    const usize n = pts.size();
    if (n < 3) return;
    const std::vector<PointF> tan = curve_tangents(pts, tension, true);
    path.new_figure = true;
    connect_first(path, pts[0]);
    for (usize s = 0; s < n; ++s) {
        const PointF a = pts[s], b = pts[(s + 1) % n];
        const PointF ta = tan[s], tb = tan[(s + 1) % n];
        push(path, {a.X + ta.X, a.Y + ta.Y}, PathPointTypeBezier);
        push(path, {b.X - tb.X, b.Y - tb.Y}, PathPointTypeBezier);
        push(path, b, PathPointTypeBezier);
    }
    close_last(path);
}

void path_add_rectangle(GpPath& path, RectF r) {
    // "rect(1,2,0,40)", "rect(1,2,-30,40)": empty rectangles add nothing.
    if (!(r.Width > 0) || !(r.Height > 0)) return;
    path.new_figure = true;
    connect_first(path, {r.X, r.Y});
    push(path, {r.X + r.Width, r.Y}, PathPointTypeLine);
    push(path, {r.X + r.Width, r.Y + r.Height}, PathPointTypeLine);
    push(path, {r.X, r.Y + r.Height}, PathPointTypeLine);
    close_last(path);
}

void path_add_ellipse(GpPath& path, RectF r) {
    // Any size, zero or negative included ("ellipse(0,0,-10,10)").
    const float rx = r.Width / 2, ry = r.Height / 2;
    const float cx = r.X + rx, cy = r.Y + ry;
    const float kx = rx * kEllipseK, ky = ry * kEllipseK;
    path.new_figure = true;
    connect_first(path, {cx + rx, cy});
    const PointF rest[12] = {
        {cx + rx, cy + ky}, {cx + kx, cy + ry}, {cx, cy + ry},
        {cx - kx, cy + ry}, {cx - rx, cy + ky}, {cx - rx, cy},
        {cx - rx, cy - ky}, {cx - kx, cy - ry}, {cx, cy - ry},
        {cx + kx, cy - ry}, {cx + rx, cy - ky}, {cx + rx, cy},
    };
    for (const PointF& p : rest) push(path, p, PathPointTypeBezier);
    close_last(path);
}

void path_add_pie(GpPath& path, RectF r, float start, float sweep) {
    // Centre, a line to the arc, the arc, closed ("pie(10,20,100,50,30,120)").
    // The centre is added before the rectangle is checked, so an invalid
    // rectangle leaves the closed centre point ("pie(0,0,0,10,0,90)").
    path.new_figure = true;
    connect_first(path, {r.X + r.Width / 2, r.Y + r.Height / 2});
    if (r.Width > 0 && r.Height > 0) path_add_arc(path, r, start, sweep);
    close_last(path);
}

void path_add_polygon(GpPath& path, const std::vector<PointF>& pts) {
    usize n = pts.size();
    if (n < 3) return;
    // A last point repeating the first is dropped ("poly_firstlast").
    if (same_point(pts[0], pts[n - 1])) --n;
    path.new_figure = true;
    connect_first(path, pts[0]);
    for (usize i = 1; i < n; ++i) push(path, pts[i], PathPointTypeLine);
    close_last(path);
}

void path_close_figure(GpPath& path) {
    // No-op on an empty path or with no figure open ("close_after_start").
    if (path.points.empty() || path.new_figure) return;
    close_last(path);
}

void path_transform(GpPath& path, const Affine& m) {
    for (PointF& p : path.points) p = m.apply(p);
}

CGMutablePathRef path_to_cg(const GpPath& path) {
    CGMutablePathRef cg = CGPathCreateMutable();
    const usize n = path.points.size();
    for (usize i = 0; i < n; ++i) {
        const u8 t = path.types[i];
        const PointF p = path.points[i];
        switch (t & PathPointTypePathTypeMask) {
        case PathPointTypeStart:
            CGPathMoveToPoint(cg, nullptr, p.X, p.Y);
            break;
        case PathPointTypeBezier:
            if (i + 2 < n) {
                const PointF b = path.points[i + 1], c = path.points[i + 2];
                CGPathAddCurveToPoint(cg, nullptr, p.X, p.Y, b.X, b.Y, c.X, c.Y);
                i += 2;
            } else {
                CGPathAddLineToPoint(cg, nullptr, p.X, p.Y);
            }
            break;
        default:
            CGPathAddLineToPoint(cg, nullptr, p.X, p.Y);
            break;
        }
        if (path.types[i] & PathPointTypeCloseSubpath) CGPathCloseSubpath(cg);
    }
    return cg;
}

void flatten_path(const GpPath& in, const Affine* m, float flatness, GpPath& out) {
    GpPath src = in;
    if (m != nullptr) path_transform(src, *m);
    GpPath res;
    res.fill_mode = in.fill_mode;
    res.new_figure = in.new_figure;
    const usize n = src.points.size();
    const bool degenerate = flatness == 0;
    for (auto [s, e] : figures(src)) {
        // The figure's start is on the curve grid when a curve follows it
        // ("flatten_neg_bez"); line end points are kept exactly
        // ("flatten_frac_lines").
        PointF first = src.points[s];
        const bool curve_next = s + 1 <= e &&
            (src.types[s + 1] & PathPointTypePathTypeMask) == PathPointTypeBezier;
        if (degenerate) {
            first = {0, 0};
        } else if (curve_next) {
            first = {snap_flat(first.X, flatness), snap_flat(first.Y, flatness)};
        }
        res.points.push_back(first);
        res.types.push_back(static_cast<u8>(src.types[s] & PathPointTypeDashMode));
        usize i = s + 1;
        while (i <= e) {
            const u8 t = src.types[i];
            if ((t & PathPointTypePathTypeMask) == PathPointTypeBezier && i + 2 <= e) {
                const u8 last = src.types[i + 2];
                // Markers do not survive flattening ("flatten_mixed": 0x81).
                const u8 flags = last & PathPointTypeCloseSubpath;
                const u8 dash = t & PathPointTypeDashMode;
                if (degenerate) {
                    // "flatten_line_bez(0)": a curve collapses to one point at
                    // the origin (the grid scale is infinite).
                    res.points.push_back({0, 0});
                    res.types.push_back(static_cast<u8>(PathPointTypeLine | dash | flags));
                } else {
                    const PointF ctl[4] = {src.points[i - 1], src.points[i], src.points[i + 1],
                                           src.points[i + 2]};
                    std::vector<PointF> pts;
                    flatten_bezier(ctl, flatness, pts);
                    for (usize k = 0; k < pts.size(); ++k) {
                        res.points.push_back(pts[k]);
                        res.types.push_back(static_cast<u8>(
                            PathPointTypeLine | dash | (k + 1 == pts.size() ? flags : 0)));
                    }
                }
                i += 3;
            } else {
                res.points.push_back(degenerate ? PointF{0, 0} : src.points[i]);
                res.types.push_back(static_cast<u8>(
                    (t & (PathPointTypeCloseSubpath | PathPointTypeDashMode)) | PathPointTypeLine));
                ++i;
            }
        }
    }
    (void)n;
    out = std::move(res);
}

RectF path_bounds(const GpPath& path, const Affine* m) {
    if (path.points.empty()) return {};
    float x0 = path.points[0].X, y0 = path.points[0].Y, x1 = x0, y1 = y0;
    for (PointF p : path.points) {
        x0 = std::min(x0, p.X);
        y0 = std::min(y0, p.Y);
        x1 = std::max(x1, p.X);
        y1 = std::max(y1, p.Y);
    }
    if (m == nullptr) return {x0, y0, x1 - x0, y1 - y0};
    // GDI+ maps the corners of the untransformed bounds, not the points
    // ("bounds_ellipse_m": 12.5,15,212.5,125 for a 2:1 sheared ellipse).
    const PointF c[4] = {m->apply({x0, y0}), m->apply({x1, y0}), m->apply({x0, y1}),
                         m->apply({x1, y1})};
    float a0 = c[0].X, b0 = c[0].Y, a1 = a0, b1 = b0;
    for (PointF p : c) {
        a0 = std::min(a0, p.X);
        b0 = std::min(b0, p.Y);
        a1 = std::max(a1, p.X);
        b1 = std::max(b1, p.Y);
    }
    return {a0, b0, a1 - a0, b1 - b0};
}

// ---- Reverse ---------------------------------------------------------------

void path_reverse(GpPath& path) {
    const usize n = path.points.size();
    if (n == 0) return;
    // Rules from "reversed", "reverse_mixed" and "rev_dash": the segment
    // type (Line/Bezier) belongs to the segment, so reversed point j takes
    // the base type of the original point that followed it; the dash bit
    // stays with its point; a marker after original point i moves after
    // reversed point n-2-i; each figure keeps its close flag on its new
    // last point.
    std::vector<PointF> pts(path.points.rbegin(), path.points.rend());
    std::vector<u8> types(n, 0);
    std::vector<bool> marker(n, false);
    for (usize i = 0; i + 1 < n; ++i) {
        if (path.types[i] & PathPointTypePathMarker) marker[n - 2 - i] = true;
    }
    for (auto [s, e] : figures(path)) {
        const bool closed = (path.types[e] & PathPointTypeCloseSubpath) != 0;
        const usize rs = n - 1 - e, re = n - 1 - s;
        for (usize j = rs; j <= re; ++j) {
            const usize orig = n - 1 - j;
            const u8 dash = path.types[orig] & PathPointTypeDashMode;
            u8 base = PathPointTypeStart;
            if (j != rs) base = path.types[orig + 1] & PathPointTypePathTypeMask;
            types[j] = static_cast<u8>(base | dash);
        }
        if (closed) types[re] |= PathPointTypeCloseSubpath;
    }
    for (usize j = 0; j < n; ++j) {
        if (marker[j]) types[j] |= PathPointTypePathMarker;
    }
    path.points = std::move(pts);
    path.types = std::move(types);
}

// ---- Containment -----------------------------------------------------------

bool path_contains_point(const GpPath& path, PointF pt, bool winding) {
    if (path.points.empty()) return false;
    GpPath flat;
    flatten_path(path, nullptr, kFlatnessDefault, flat);
    // A tiny offset toward +x/+y puts an edge point inside on a left/top
    // edge and outside on a right/bottom one (the GDI+ fill convention).
    const PointD p{pt.X + 1.0 / 4096.0, pt.Y + 1.0 / 4096.0};
    const int w = winding_number(polygons(flat), p);
    return winding ? w != 0 : (w & 1) != 0;
}

// ---- Winding outline -------------------------------------------------------

namespace {

struct Seg {
    PointD a, b;
    usize order;  ///< original segment index, for output order
};

}  // namespace

bool winding_outline(GpPath& path, const Affine* m, float flatness) {
    GpPath flat;
    flatten_path(path, m, flatness, flat);
    const auto polys = polygons(flat);
    std::vector<Seg> segs;
    for (const auto& poly : polys) {
        const usize n = poly.size();
        if (n < 2) continue;
        for (usize i = 0; i < n; ++i) {
            const PointD a = poly[i], b = poly[(i + 1) % n];
            if (a.x == b.x && a.y == b.y) continue;
            segs.push_back({a, b, segs.size()});
        }
    }
    // Split every segment where another one crosses or touches it.
    std::vector<std::vector<double>> cuts(segs.size());
    for (usize i = 0; i < segs.size(); ++i) {
        cuts[i].push_back(0.0);
        cuts[i].push_back(1.0);
    }
    for (usize i = 0; i < segs.size(); ++i) {
        for (usize j = i + 1; j < segs.size(); ++j) {
            const PointD p = segs[i].a, r{segs[i].b.x - p.x, segs[i].b.y - p.y};
            const PointD q = segs[j].a, s{segs[j].b.x - q.x, segs[j].b.y - q.y};
            const double rxs = r.x * s.y - r.y * s.x;
            const PointD qp{q.x - p.x, q.y - p.y};
            if (rxs == 0) {
                if (qp.x * r.y - qp.y * r.x != 0) continue;  // parallel, apart
                // Collinear: each one's end points cut the other.
                auto param = [](PointD o, PointD d, PointD x) {
                    const double dd = d.x * d.x + d.y * d.y;
                    return ((x.x - o.x) * d.x + (x.y - o.y) * d.y) / dd;
                };
                for (PointD x : {segs[j].a, segs[j].b}) {
                    const double t = param(p, r, x);
                    if (t > 0 && t < 1) cuts[i].push_back(t);
                }
                for (PointD x : {segs[i].a, segs[i].b}) {
                    const double t = param(q, s, x);
                    if (t > 0 && t < 1) cuts[j].push_back(t);
                }
                continue;
            }
            const double t = (qp.x * s.y - qp.y * s.x) / rxs;
            const double u = (qp.x * r.y - qp.y * r.x) / rxs;
            if (t < 0 || t > 1 || u < 0 || u > 1) continue;
            if (t > 0 && t < 1) cuts[i].push_back(t);
            if (u > 0 && u < 1) cuts[j].push_back(u);
        }
    }
    struct Piece {
        PointD a, b;
        usize order;
        double t;
    };
    std::vector<Piece> kept;
    for (usize i = 0; i < segs.size(); ++i) {
        auto& c = cuts[i];
        std::sort(c.begin(), c.end());
        c.erase(std::unique(c.begin(), c.end()), c.end());
        const Seg& sg = segs[i];
        auto at = [&](double t) {
            if (t == 0) return sg.a;
            if (t == 1) return sg.b;
            return PointD{sg.a.x + (sg.b.x - sg.a.x) * t, sg.a.y + (sg.b.y - sg.a.y) * t};
        };
        for (usize k = 0; k + 1 < c.size(); ++k) {
            const PointD a = at(c[k]), b = at(c[k + 1]);
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double len = std::hypot(dx, dy);
            if (len == 0) continue;
            const double eps = std::min(1e-3, len * 1e-3);
            const PointD mid{(a.x + b.x) / 2, (a.y + b.y) / 2};
            const PointD left{mid.x - dy / len * eps, mid.y + dx / len * eps};
            const PointD right{mid.x + dy / len * eps, mid.y - dx / len * eps};
            const bool in_l = winding_number(polys, left) != 0;
            const bool in_r = winding_number(polys, right) != 0;
            if (in_l == in_r) continue;
            // Keep the area on the edge's left (math orientation), the side
            // the source figure's own interior lies on ("winding_hole").
            if (in_l) kept.push_back({a, b, sg.order, c[k]});
            else kept.push_back({b, a, sg.order, c[k]});
        }
    }
    if (kept.empty()) return false;
    // Link the kept edges into loops, in source order.
    auto key = [](PointD p) {
        return std::make_pair(std::llround(p.x * 1048576.0), std::llround(p.y * 1048576.0));
    };
    std::multimap<std::pair<long long, long long>, usize> from;
    for (usize i = 0; i < kept.size(); ++i) from.emplace(key(kept[i].a), i);
    std::vector<usize> by_order(kept.size());
    for (usize i = 0; i < kept.size(); ++i) by_order[i] = i;
    std::sort(by_order.begin(), by_order.end(), [&](usize x, usize y) {
        return kept[x].order != kept[y].order ? kept[x].order < kept[y].order
                                              : kept[x].t < kept[y].t;
    });
    std::vector<bool> used(kept.size(), false);
    GpPath out;
    out.fill_mode = path.fill_mode;
    for (usize start : by_order) {
        if (used[start]) continue;
        std::vector<usize> loop;
        usize cur = start;
        while (true) {
            used[cur] = true;
            loop.push_back(cur);
            const auto k = key(kept[cur].b);
            if (k == key(kept[start].a)) break;
            usize next = kept.size();
            auto range = from.equal_range(k);
            // Prefer the continuation of the same source segment, else the
            // first unused edge leaving this vertex in source order.
            for (auto it = range.first; it != range.second; ++it) {
                if (used[it->second]) continue;
                if (next == kept.size() || kept[it->second].order < kept[next].order) {
                    next = it->second;
                }
            }
            if (next == kept.size()) break;
            cur = next;
        }
        // Emit, merging collinear pieces of one source segment.
        std::vector<PointD> pts;
        std::vector<usize> ord;
        for (usize idx : loop) {
            const Piece& pc = kept[idx];
            // A piece continuing the same source segment starts at a split
            // point in the middle of a straight edge: not a vertex.
            if (!ord.empty() && ord.back() == pc.order) continue;
            pts.push_back(pc.a);
            ord.push_back(pc.order);
        }
        if (pts.size() > 1 && ord.front() == ord.back()) {
            pts.erase(pts.begin());
            ord.erase(ord.begin());
        }
        if (pts.size() < 3) continue;
        for (usize i = 0; i < pts.size(); ++i) {
            out.points.push_back({static_cast<float>(pts[i].x), static_cast<float>(pts[i].y)});
            out.types.push_back(i == 0 ? PathPointTypeStart : PathPointTypeLine);
        }
        out.types.back() |= PathPointTypeCloseSubpath;
    }
    if (out.points.empty()) return false;
    out.new_figure = true;
    path = std::move(out);
    return true;
}

// ---- Warp ------------------------------------------------------------------

void warp_path(GpPath& path, const Affine* m, const PointF* dst, i32 dst_count, RectF src,
               i32 mode, float flatness) {
    PointD d[4] = {{dst[0].X, dst[0].Y}, {dst[1].X, dst[1].Y}, {dst[2].X, dst[2].Y}, {}};
    // Anything but 4 points is a parallelogram on the first three
    // ("warp3", "warp_count1", "warp_count5").
    if (dst_count == 4) d[3] = {dst[3].X, dst[3].Y};
    else d[3] = {d[1].x + d[2].x - d[0].x, d[1].y + d[2].y - d[0].y};
    // GDI+ does the arithmetic in REAL: a zero-sized source rectangle makes
    // every point NaN ("warp_w0").
    const float sx = src.X, sy = src.Y, sw = src.Width, sh = src.Height;
    auto uv = [&](PointF p) { return PointD{(p.X - sx) / sw, (p.Y - sy) / sh}; };
    // Perspective: the projective map of the unit square onto d[0] (0,0),
    // d[1] (1,0), d[2] (0,1), d[3] (1,1).
    double a = 0, b = 0, c = 0, dd = 0, e = 0, f = 0, g = 0, h = 0;
    {
        const PointD p00 = d[0], p10 = d[1], p01 = d[2], p11 = d[3];
        const double ssx = p00.x - p10.x + p11.x - p01.x;
        const double ssy = p00.y - p10.y + p11.y - p01.y;
        if (ssx == 0 && ssy == 0) {
            a = p10.x - p00.x; b = p01.x - p00.x; c = p00.x;
            dd = p10.y - p00.y; e = p01.y - p00.y; f = p00.y;
        } else {
            const double dx1 = p10.x - p11.x, dx2 = p01.x - p11.x;
            const double dy1 = p10.y - p11.y, dy2 = p01.y - p11.y;
            const double det = dx1 * dy2 - dx2 * dy1;
            g = (ssx * dy2 - dx2 * ssy) / det;
            h = (dx1 * ssy - ssx * dy1) / det;
            a = p10.x - p00.x + g * p10.x; b = p01.x - p00.x + h * p01.x; c = p00.x;
            dd = p10.y - p00.y + g * p10.y; e = p01.y - p00.y + h * p01.y; f = p00.y;
        }
    }
    auto persp = [&](PointD q) {
        const double w = g * q.x + h * q.y + 1;
        return PointF{static_cast<float>((a * q.x + b * q.y + c) / w),
                      static_cast<float>((dd * q.x + e * q.y + f) / w)};
    };
    auto bilinear = [&](PointD q) {
        const double u = q.x, v = q.y;
        return PointF{static_cast<float>((1 - u) * (1 - v) * d[0].x + u * (1 - v) * d[1].x +
                                         (1 - u) * v * d[2].x + u * v * d[3].x),
                      static_cast<float>((1 - u) * (1 - v) * d[0].y + u * (1 - v) * d[1].y +
                                         (1 - u) * v * d[2].y + u * v * d[3].y)};
    };
    GpPath flat;
    flatten_path(path, nullptr, flatness, flat);
    GpPath out;
    out.fill_mode = path.fill_mode;
    out.new_figure = true;
    const PointD kq{d[0].x - d[1].x - d[2].x + d[3].x, d[0].y - d[1].y - d[2].y + d[3].y};
    for (auto [s, e2] : figures(flat)) {
        for (usize i = s; i <= e2; ++i) {
            // Markers do not survive a warp ("warp_persp_marker"); a bilinear
            // warp also drops the close flags ("warp_bilinear", "warp3_bilinear").
            u8 t = static_cast<u8>(flat.types[i] & ~PathPointTypePathMarker);
            if (mode == 1) t = static_cast<u8>(t & ~PathPointTypeCloseSubpath);
            const PointD q = uv(flat.points[i]);
            if (mode == 1 && i > s) {
                // The bilinear image of a line is a parabola: subdivide it into
                // 2^k pieces until each chord is within 1/4 of the curve.
                const PointD q0 = uv(flat.points[i - 1]);
                const double du = q.x - q0.x, dv = q.y - q0.y;
                const double dev = std::max(std::fabs(du * dv * kq.x), std::fabs(du * dv * kq.y));
                int pieces = 1;
                while (pieces < 1024 && dev / (4.0 * pieces * pieces) > 0.25) pieces *= 2;
                for (int k = 1; k < pieces; ++k) {
                    const double r = static_cast<double>(k) / pieces;
                    out.points.push_back(bilinear({q0.x + du * r, q0.y + dv * r}));
                    out.types.push_back(PathPointTypeLine);
                }
            }
            out.points.push_back(mode == 1 ? bilinear(q) : persp(q));
            out.types.push_back(t);
        }
    }
    if (m != nullptr) path_transform(out, *m);
    path = std::move(out);
}

}  // namespace vitreo::winapi::gdip
