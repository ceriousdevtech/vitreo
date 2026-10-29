// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_pen.cpp
/// @brief GDI+ area (draw): pens, custom/adjustable line caps, and the
/// stroker (widen_path / stroke_path). Registered from register_draw.
///
/// Every default, error code and widened shape below is measured by
/// tools/oracle_probe/gdiplus_draw_probe.c on winoracle (x86 and x64, see
/// the .expected files); the cases are listed in .docs/contracts/gdiplus/draw.md.

#include "winapi/graphics/gdiplus/gdiplus_pen.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_geometry.hpp"
#include "winapi/graphics/gdiplus/gdiplus_graphics.hpp"
#include "winapi/graphics/gdiplus/gdiplus_pen_brush.hpp"
#include "winapi/graphics/gdiplus/gdiplus_render.hpp"
#include "winapi/core/marshaler.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>

namespace vitreo::winapi::gdip {

// ---- Shared helpers ---------------------------------------------------------

std::vector<float> dash_pattern(i32 style) {
    switch (style) {
    case DashStyleDash: return {3, 1};
    case DashStyleDot: return {1, 1};
    case DashStyleDashDot: return {3, 1, 1, 1};
    case DashStyleDashDotDot: return {3, 1, 1, 1, 1, 1};
    default: return {};
    }
}

std::shared_ptr<GpCustomLineCap> clone_cap(const GpCustomLineCap& cap) {
    auto c = std::make_shared<GpCustomLineCap>(cap);
    if (cap.fill_path) c->fill_path = std::make_shared<GpPath>(*cap.fill_path);
    if (cap.stroke_path) c->stroke_path = std::make_shared<GpPath>(*cap.stroke_path);
    return c;
}

GpPen clone_pen(const GpPen& pen) {
    GpPen p = pen;
    if (pen.brush) p.brush = std::make_shared<GpBrush>(*pen.brush);
    if (pen.custom_start) p.custom_start = clone_cap(*pen.custom_start);
    if (pen.custom_end) p.custom_end = clone_cap(*pen.custom_end);
    return p;
}

Status resolve_status(GdiplusManager& mgr, u64 h, bool found) {
    if (h == 0) return Status::InvalidParameter;
    if (found) return Status::Ok;
    // A live GDI+ object of another class: the probe (drawline
    // brush-as-pen, graphics-as-pen, fillpath pen) measures ObjectBusy.
    const ObjectTable& t = mgr.objects();
    const bool other = t.get<GpPen>(h) != nullptr || t.get<GpBrush>(h) != nullptr ||
                       t.get<GpGraphics>(h) != nullptr || t.get<GpPath>(h) != nullptr ||
                       t.get<GpMatrix>(h) != nullptr || t.get<GpImage>(h) != nullptr ||
                       t.get<GpRegion>(h) != nullptr || t.get<GpCustomLineCap>(h) != nullptr ||
                       t.get<GpPathIterator>(h) != nullptr;
    return other ? Status::ObjectBusy : Status::InvalidParameter;
}

namespace {

// ---- Geometry primitives ----------------------------------------------------

struct D2 {
    double x{0}, y{0};
};
inline D2 operator+(D2 a, D2 b) { return {a.x + b.x, a.y + b.y}; }
inline D2 operator-(D2 a, D2 b) { return {a.x - b.x, a.y - b.y}; }
inline D2 operator*(D2 a, double s) { return {a.x * s, a.y * s}; }
inline double dot(D2 a, D2 b) { return a.x * b.x + a.y * b.y; }
inline double cross(D2 a, D2 b) { return a.x * b.y - a.y * b.x; }
inline double len(D2 a) { return std::sqrt(dot(a, a)); }
inline D2 unit(D2 a) {
    const double l = len(a);
    return l > 0 ? a * (1.0 / l) : D2{1, 0};
}
/// The right-hand normal in GDI+ (y-down) space: offset t >= 0 is this
/// side. The first side GdipWidenPath emits is t = -width/2.
inline D2 rnorm(D2 d) { return {-d.y, d.x}; }

constexpr double kPi = 3.14159265358979323846;
constexpr double kEps = 1e-9;

struct Lin {  ///< a 2x2 linear map, GDI+ row-vector order (as Affine without dx/dy)
    double m11{1}, m12{0}, m21{0}, m22{1};
    [[nodiscard]] D2 apply(D2 p) const { return {m11 * p.x + m21 * p.y, m12 * p.x + m22 * p.y}; }
    [[nodiscard]] double det() const { return m11 * m22 - m12 * m21; }
    [[nodiscard]] Lin inverse() const {
        const double d = det();
        return {m22 / d, -m12 / d, -m21 / d, m11 / d};
    }
    /// Largest singular value: how far the map stretches a unit circle.
    [[nodiscard]] double stretch() const {
        const double a = m11 * m11 + m12 * m12, b = m11 * m21 + m12 * m22,
                     c = m21 * m21 + m22 * m22;
        const double tr = a + c, disc = std::sqrt(std::max(0.0, (a - c) * (a - c) + 4 * b * b));
        return std::sqrt(std::max(0.0, (tr + disc) / 2));
    }
};
Lin lin_of(const Affine& a) { return {a.m11, a.m12, a.m21, a.m22}; }

/// A flattened figure.
struct Figure {
    std::vector<D2> pts;
    bool closed{false};
};

std::vector<Figure> figures_of(const GpPath& flat) {
    std::vector<Figure> out;
    for (usize i = 0; i < flat.points.size(); ++i) {
        const u8 t = i < flat.types.size() ? flat.types[i] : PathPointTypeLine;
        if ((t & PathPointTypePathTypeMask) == PathPointTypeStart || out.empty()) out.push_back({});
        const D2 p{flat.points[i].X, flat.points[i].Y};
        Figure& f = out.back();
        if (f.pts.empty() || len(p - f.pts.back()) > 1e-6) f.pts.push_back(p);
        if (t & PathPointTypeCloseSubpath) f.closed = true;
    }
    for (Figure& f : out) {
        if (f.closed && f.pts.size() > 1 && len(f.pts.front() - f.pts.back()) <= 1e-6) {
            f.pts.pop_back();
        }
    }
    return out;
}

/// Accumulates a GpPath.
struct Emit {
    GpPath& path;
    bool open{false};
    void start(D2 p) {
        path.points.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
        path.types.push_back(PathPointTypeStart);
        open = true;
    }
    void line(D2 p) {
        path.points.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
        path.types.push_back(PathPointTypeLine);
    }
    void bez(D2 a, D2 b, D2 c) {
        for (D2 p : {a, b, c}) {
            path.points.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
            path.types.push_back(PathPointTypeBezier);
        }
    }
    void close() {
        if (open && !path.types.empty()) path.types.back() |= PathPointTypeCloseSubpath;
        open = false;
    }
};

/// One point of an outline side: a line vertex, or the three points of a
/// cubic Bézier (control, control, end).
struct Op {
    D2 p;
    u8 type;  ///< PathPointTypeLine or PathPointTypeBezier
};
using Seq = std::vector<Op>;

/// Arc around @p c from the direction of (@p from - c) by @p sweep radians,
/// as cubic Béziers of at most 90 degrees each (GDI+ emits 2 for a round
/// cap, 1 for a right-angle round join: probe w1-endcap-0x2, right-join2).
void arc_to(Seq& seq, D2 c, D2 from, double sweep) {
    const double r = len(from - c);
    double a = std::atan2(from.y - c.y, from.x - c.x);
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / (kPi / 2) - 1e-6)));
    const double step = sweep / n;
    const double k = 4.0 / 3.0 * std::tan(step / 4);
    for (int i = 0; i < n; ++i) {
        const double b = a + step;
        const D2 p0{c.x + r * std::cos(a), c.y + r * std::sin(a)};
        const D2 p3{c.x + r * std::cos(b), c.y + r * std::sin(b)};
        const D2 t0{-std::sin(a), std::cos(a)}, t1{-std::sin(b), std::cos(b)};
        seq.push_back({p0 + t0 * (k * r), PathPointTypeBezier});
        seq.push_back({p3 - t1 * (k * r), PathPointTypeBezier});
        seq.push_back({p3, PathPointTypeBezier});
        a = b;
    }
}

enum class InnerJoin {
    Endpoints,     ///< centre outline: the two offset end points (probe right-join*)
    Intersection,  ///< compound bands / inset loops: the offset lines meet (square-compound)
};

struct Style {
    double hw{0.5};          ///< half the pen width
    i32 join{LineJoinMiter};
    double miter_limit{10};
};

/// The points of offset side @p t at vertex @p v between directions @p d1
/// and @p d2 (probe vee-join*/right-join*: Miter clips at miter_limit*hw
/// from the vertex, MiterClipped bevels, Bevel/Round as named; the inner
/// side joins per @p inner).
void join(Seq& seq, D2 v, D2 d1, D2 d2, double t, const Style& st, InnerJoin inner) {
    const D2 r1 = rnorm(d1), r2 = rnorm(d2);
    const D2 a = v + r1 * t, b = v + r2 * t;
    const double cr = cross(d1, d2), dt = dot(d1, d2);
    if (std::fabs(t) < kEps) { seq.push_back({v, PathPointTypeLine}); return; }
    if (std::fabs(cr) < 1e-9 && dt > 0) { seq.push_back({a, PathPointTypeLine}); return; }
    const bool uturn = std::fabs(cr) < 1e-9;
    const bool outer = uturn || t * cr < 0;
    const double denom = 1 + dot(r1, r2);
    if (!outer) {
        if (inner == InnerJoin::Intersection && denom > 1e-6) {
            seq.push_back({v + (r1 + r2) * (t / denom), PathPointTypeLine});
        } else {
            seq.push_back({a, PathPointTypeLine});
            seq.push_back({b, PathPointTypeLine});
        }
        return;
    }
    switch (st.join) {
    case LineJoinBevel:
        seq.push_back({a, PathPointTypeLine});
        seq.push_back({b, PathPointTypeLine});
        return;
    case LineJoinRound: {
        seq.push_back({a, PathPointTypeLine});
        double sweep = std::atan2(cross(a - v, b - v), dot(a - v, b - v));
        if (uturn) sweep = t < 0 ? kPi : -kPi;
        arc_to(seq, v, a, sweep);
        return;
    }
    default: break;
    }
    // Miter (LineJoinMiter and any other stored value) / MiterClipped.
    const double limit = st.miter_limit * st.hw;
    if (denom > 1e-6) {
        const D2 mpt = v + (r1 + r2) * (t / denom);
        if (len(mpt - v) <= limit + 1e-9) { seq.push_back({mpt, PathPointTypeLine}); return; }
    }
    if (st.join == LineJoinMiterClipped || uturn) {
        seq.push_back({a, PathPointTypeLine});
        seq.push_back({b, PathPointTypeLine});
        return;
    }
    const D2 m = unit(r1 + r2) * (t < 0 ? -1.0 : 1.0);
    const double k1 = dot(d1, m), k2 = dot(d2, m);
    const D2 p1 = std::fabs(k1) > kEps ? a + d1 * ((limit - dot(a - v, m)) / k1) : a;
    const D2 p2 = std::fabs(k2) > kEps ? b + d2 * ((limit - dot(b - v, m)) / k2) : b;
    seq.push_back({p1, PathPointTypeLine});
    seq.push_back({p2, PathPointTypeLine});
}

std::vector<D2> dirs_of(const std::vector<D2>& p, bool closed) {
    std::vector<D2> d;
    const usize n = p.size();
    const usize segs = closed ? n : n - 1;
    for (usize i = 0; i < segs; ++i) d.push_back(unit(p[(i + 1) % n] - p[i]));
    return d;
}

/// The offset-@p t side of open polyline @p p, start to end.
Seq open_side(const std::vector<D2>& p, double t, const Style& st, InnerJoin inner) {
    const std::vector<D2> d = dirs_of(p, false);
    Seq seq;
    seq.push_back({p[0] + rnorm(d[0]) * t, PathPointTypeLine});
    for (usize i = 1; i + 1 < p.size(); ++i) join(seq, p[i], d[i - 1], d[i], t, st, inner);
    seq.push_back({p.back() + rnorm(d.back()) * t, PathPointTypeLine});
    return seq;
}

/// The offset-@p t loop of closed polygon @p p: a join at every vertex,
/// starting at vertex 0 (probe closed-round-join, square-center).
Seq closed_side(const std::vector<D2>& p, double t, const Style& st, InnerJoin inner) {
    const std::vector<D2> d = dirs_of(p, true);
    const usize n = p.size();
    Seq seq;
    for (usize i = 0; i < n; ++i) join(seq, p[i], d[(i + n - 1) % n], d[i], t, st, inner);
    return seq;
}

void emit_seq(Emit& e, const Seq& seq, bool first_starts) {
    for (usize i = 0; i < seq.size(); ++i) {
        if (i == 0 && first_starts) e.start(seq[i].p);
        else if (seq[i].type == PathPointTypeBezier) {
            e.bez(seq[i].p, seq[i + 1].p, seq[i + 2].p);
            i += 2;
        } else e.line(seq[i].p);
    }
}

std::vector<D2> reversed(std::vector<D2> p) {
    std::reverse(p.begin(), p.end());
    return p;
}

/// Simple caps drawn as part of the outline (Flat/Square/Round/Triangle).
/// Anchor, custom and unknown cap values are flat here; anchors and custom
/// caps add their own figures.
i32 outline_cap(i32 cap) {
    return (cap == LineCapSquare || cap == LineCapRound || cap == LineCapTriangle) ? cap
                                                                                    : LineCapFlat;
}

/// The cap points between side end @p from (at end point @p e, outward
/// direction @p d) and the opposite side end, for Round / Triangle.
void cap_points(Seq& seq, i32 cap, D2 e, D2 d, D2 from, double hw) {
    if (cap == LineCapRound) arc_to(seq, e, from, kPi);
    else if (cap == LineCapTriangle) seq.push_back({e + d * hw, PathPointTypeLine});
}

/// One open piece (a figure, or a dash of one) as a closed outline of band
/// [ta, tb] with caps @p start_cap / @p end_cap (outline caps only).
void open_outline(Emit& e, const std::vector<D2>& p, double ta, double tb, i32 start_cap,
                  i32 end_cap, const Style& st, InnerJoin inner) {
    const std::vector<D2> d = dirs_of(p, false);
    Seq fwd = open_side(p, ta, st, inner);
    Seq back = open_side(reversed(p), -tb, st, inner);
    const D2 d0 = d.front(), dn = d.back();
    const D2 s = p.front(), en = p.back();
    if (start_cap == LineCapSquare) {
        fwd.front().p = fwd.front().p - d0 * st.hw;
        back.back().p = back.back().p - d0 * st.hw;
    }
    if (end_cap == LineCapSquare) {
        fwd.back().p = fwd.back().p + dn * st.hw;
        back.front().p = back.front().p + dn * st.hw;
    }
    Seq all = fwd;
    cap_points(all, end_cap, en, dn, fwd.back().p, st.hw);
    // A round end cap's arc ends on back.front() itself.
    all.insert(all.end(), back.begin() + (end_cap == LineCapRound ? 1 : 0), back.end());
    if (start_cap == LineCapRound || start_cap == LineCapTriangle) {
        cap_points(all, start_cap, s, d0 * -1.0, back.back().p, st.hw);
        if (start_cap == LineCapTriangle) all.push_back({fwd.front().p, PathPointTypeLine});
    }
    emit_seq(e, all, true);
    e.close();
}

/// Trim @p dist from the start (@p at_start) or end of polyline @p p.
/// False when nothing is left.
bool trim(std::vector<D2>& p, double dist, bool at_start) {
    if (dist <= 0) return true;
    if (at_start) std::reverse(p.begin(), p.end());
    while (p.size() >= 2) {
        const double l = len(p.back() - p[p.size() - 2]);
        if (l > dist + 1e-9) {
            p.back() = p.back() + unit(p[p.size() - 2] - p.back()) * dist;
            break;
        }
        dist -= l;
        p.pop_back();
    }
    if (at_start) std::reverse(p.begin(), p.end());
    return p.size() >= 2;
}

struct Dash {
    std::vector<D2> pts;
    bool at_start{false};
    bool at_end{false};
};

double poly_len(const std::vector<D2>& p) {
    double l = 0;
    for (usize i = 1; i < p.size(); ++i) l += len(p[i] - p[i - 1]);
    return l;
}

/// The sub-polyline of @p p between arc lengths @p s0 and @p s1; when
/// s1 == s0 a tiny piece keeps the local direction.
std::vector<D2> sub_poly(const std::vector<D2>& p, double s0, double s1) {
    if (s1 - s0 < 1e-6) s1 = s0 + 1e-6;
    std::vector<D2> out;
    double acc = 0;
    for (usize i = 1; i < p.size(); ++i) {
        const double l = len(p[i] - p[i - 1]);
        const double a = acc, b = acc + l;
        acc = b;
        if (l <= 0 || b < s0 || a > s1) continue;
        const D2 dir = (p[i] - p[i - 1]) * (1.0 / l);
        const D2 q0 = p[i - 1] + dir * std::max(0.0, s0 - a);
        const D2 q1 = p[i - 1] + dir * std::min(l, s1 - a);
        if (out.empty()) out.push_back(q0);
        if (len(q1 - out.back()) > 1e-9 || out.size() == 1) out.push_back(q1);
    }
    if (out.size() == 1) out.push_back(out[0] + D2{1e-6, 0});
    return out;
}

/// Dash @p p with the pen's pattern (lengths in units of @p unit, offset
/// in the same units). Probe dash*: the pattern restarts per figure, a
/// dash cap shortens the dash end by the pen width (dash1-tricap), and the
/// line caps apply only to a dash touching the figure's ends.
std::vector<Dash> dash_poly(const std::vector<D2>& p, const std::vector<float>& pattern,
                            double unit_len, double offset, double shorten_end) {
    std::vector<Dash> out;
    double period = 0;
    for (float f : pattern) period += f * unit_len;
    const double total = poly_len(p);
    if (period <= 0 || total <= 0) return out;
    double phase = std::fmod(offset * unit_len, period);
    if (phase < 0) phase += period;
    usize k = 0;
    double rem = pattern[0] * unit_len;
    while (phase > 0) {
        if (phase < rem) { rem -= phase; break; }
        phase -= rem;
        k = (k + 1) % pattern.size();
        rem = pattern[k] * unit_len;
    }
    double s = 0;
    while (s < total - 1e-9) {
        const double e = std::min(total, s + rem);
        if (k % 2 == 0 && e > s) {
            Dash dsh;
            dsh.at_start = s <= 1e-9;
            dsh.at_end = e >= total - 1e-9;
            double e2 = e;
            if (!dsh.at_end && shorten_end > 0) e2 = std::max(s, e - shorten_end);
            dsh.pts = sub_poly(p, s, e2);
            out.push_back(std::move(dsh));
        }
        s = e;
        k = (k + 1) % pattern.size();
        rem = pattern[k] * unit_len;
    }
    return out;
}

double signed_area(const std::vector<D2>& p) {
    double a = 0;
    for (usize i = 0; i < p.size(); ++i) a += cross(p[i], p[(i + 1) % p.size()]);
    return a / 2;
}

double path_area(const GpPath& path, usize from) {
    double a = 0;
    usize start = from;
    for (usize i = from; i < path.points.size(); ++i) {
        const bool last = i + 1 == path.points.size() ||
                          (path.types[i + 1] & PathPointTypePathTypeMask) == PathPointTypeStart;
        if (!last) continue;
        std::vector<D2> poly;
        for (usize j = start; j <= i; ++j) poly.push_back({path.points[j].X, path.points[j].Y});
        a += signed_area(poly);
        start = i + 1;
    }
    return a;
}

/// Reverse the figures appended since @p from (to give a cap the
/// orientation the stroke outline has, so the winding union adds them).
void reverse_figures(GpPath& path, usize from) {
    usize start = from;
    for (usize i = from; i < path.points.size(); ++i) {
        const bool last = i + 1 == path.points.size() ||
                          (path.types[i + 1] & PathPointTypePathTypeMask) == PathPointTypeStart;
        if (!last) continue;
        const bool closed = (path.types[i] & PathPointTypeCloseSubpath) != 0;
        std::vector<PointF> pts(path.points.begin() + static_cast<std::ptrdiff_t>(start),
                                path.points.begin() + static_cast<std::ptrdiff_t>(i + 1));
        std::vector<u8> ty(path.types.begin() + static_cast<std::ptrdiff_t>(start),
                           path.types.begin() + static_cast<std::ptrdiff_t>(i + 1));
        const usize n = pts.size();
        for (usize j = 0; j < n; ++j) {
            path.points[start + j] = pts[n - 1 - j];
            // Segment j of the reversed figure is segment (n-1-j) reversed:
            // it keeps that segment's type (the type of its end point).
            const u8 seg = j == 0 ? PathPointTypeStart
                                  : static_cast<u8>(ty[n - j] & PathPointTypePathTypeMask);
            path.types[start + j] = seg;
        }
        if (closed) path.types[i] |= PathPointTypeCloseSubpath;
        start = i + 1;
    }
}

struct Widener {
    const GpPen& pen;
    double w;               ///< pen width in the widening space
    bool nominal_aliased;   ///< 1-pixel aliased line: both end pixels drawn
    bool for_render;        ///< orient cap figures for the winding union
    Style st;

    Widener(const GpPen& p, double width, bool nominal, bool render)
        : pen(p), w(width), nominal_aliased(nominal), for_render(render) {
        st.hw = w / 2;
        st.join = p.line_join;
        st.miter_limit = p.miter_limit;
    }

    /// Lateral bands of the pen across [ta0, ta0 + w] (compound array).
    std::vector<std::pair<double, double>> bands(double ta0) const {
        std::vector<std::pair<double, double>> b;
        if (pen.compound.size() >= 2) {
            for (usize i = 0; i + 1 < pen.compound.size(); i += 2) {
                const double a = ta0 + pen.compound[i] * w, c = ta0 + pen.compound[i + 1] * w;
                if (c - a > 1e-9) b.emplace_back(a, c);
            }
        } else {
            b.emplace_back(ta0, ta0 + w);
        }
        return b;
    }
    [[nodiscard]] bool compound() const { return pen.compound.size() >= 2; }

    void piece(Emit& e, const std::vector<D2>& p, i32 start_cap, i32 end_cap) {
        if (nominal_aliased) {
            if (start_cap == LineCapFlat) start_cap = LineCapSquare;
            if (end_cap == LineCapFlat) end_cap = LineCapSquare;
        }
        if (!compound()) {
            open_outline(e, p, -st.hw, st.hw, start_cap, end_cap, st, InnerJoin::Endpoints);
            return;
        }
        for (auto [a, b] : bands(-st.hw)) {
            open_outline(e, p, a, b, LineCapFlat, LineCapFlat, st, InnerJoin::Intersection);
        }
        // Compound pens: a Square/Round/Triangle line cap is the full-width
        // cap region beyond the end, as its own figure.
        const std::vector<D2> d = dirs_of(p, false);
        for (int end = 0; end < 2; ++end) {
            const i32 cap = end ? end_cap : start_cap;
            if (cap == LineCapFlat) continue;
            const D2 c = end ? p.back() : p.front();
            const D2 dir = end ? d.back() : d.front() * -1.0;
            const D2 l = c - rnorm(dir) * st.hw, r = c + rnorm(dir) * st.hw;
            Seq seq{{l, PathPointTypeLine}};
            if (cap == LineCapSquare) {
                seq.push_back({l + dir * st.hw, PathPointTypeLine});
                seq.push_back({r + dir * st.hw, PathPointTypeLine});
            } else {
                cap_points(seq, cap, c, dir, l, st.hw);
            }
            if (cap != LineCapRound) seq.push_back({r, PathPointTypeLine});
            emit_seq(e, seq, true);
            e.close();
        }
    }

    void closed_figure(Emit& e, const std::vector<D2>& p) {
        if (pen.alignment == PenAlignmentInset && w > 0) {
            // Inset: the band lies inside the figure (probe square-inset).
            const double base = signed_area(p) > 0 ? 0.0 : -w;
            for (auto [a, b] : bands(base)) loops(e, p, a, b, InnerJoin::Intersection);
            return;
        }
        if (!compound()) {
            loops(e, p, -st.hw, st.hw, InnerJoin::Endpoints);
            return;
        }
        for (auto [a, b] : bands(-st.hw)) loops(e, p, a, b, InnerJoin::Intersection);
    }

    void loops(Emit& e, const std::vector<D2>& p, double ta, double tb, InnerJoin inner) {
        emit_seq(e, closed_side(p, ta, st, inner), true);
        e.close();
        emit_seq(e, closed_side(reversed(p), -tb, st, inner), true);
        e.close();
    }

    // -- caps --------------------------------------------------------------

    /// Anchor caps are sized by max(width, 2) (probe r2 w0.5/w2/w8-endcap-*).
    void anchor(GpPath& out, i32 cap, D2 c, D2 d) {
        const double we = std::max(w, 2.0);
        const D2 r = rnorm(d);
        auto at = [&](double a, double p) { return c + d * a + r * p; };
        Emit e{out};
        Seq seq;
        switch (cap) {
        case LineCapSquareAnchor: {
            const double s = we / std::sqrt(2.0);
            seq = {{at(-s, s), 1}, {at(-s, -s), 1}, {at(s, -s), 1}, {at(s, s), 1}};
            break;
        }
        case LineCapRoundAnchor:
            seq.push_back({at(0, -we), PathPointTypeLine});
            arc_to(seq, c, at(0, -we), 2 * kPi);
            break;
        case LineCapDiamondAnchor:
            seq = {{at(we, 0), 1}, {at(0, we), 1}, {at(-we, 0), 1}, {at(0, -we), 1}};
            break;
        case LineCapArrowAnchor: {
            const double l = we * std::sqrt(3.0);
            seq = {{at(0, 0), 1}, {at(-l, we), 1}, {at(-l, -we), 1}};
            break;
        }
        default: return;
        }
        emit_seq(e, seq, true);
        e.close();
    }

    /// How far a cap shortens the line at its end.
    [[nodiscard]] double inset_of(i32 cap, const GpCustomLineCap* custom) const {
        if (cap == LineCapArrowAnchor) return std::max(w, 1.0);  // probe r2 w*-endcap-0x14
        if (cap == LineCapCustom && custom != nullptr) {
            return static_cast<double>(custom->base_inset) * w * custom->width_scale;
        }
        return 0;
    }

    void custom_cap(GpPath& out, const GpCustomLineCap& cap, D2 c, D2 d) {
        const double k = w * cap.width_scale;
        const D2 xax{d.y, -d.x};  // cap +X (probe r2 fillcap-end/-both)
        auto map = [&](PointF q) { return c + xax * (q.X * k) + d * (q.Y * k); };
        GpPath src;
        bool stroke = false;
        if (cap.type == CustomLineCapTypeAdjustableArrow) {
            const float hw = cap.arrow_width / 2, h = cap.arrow_height;
            src.points = {{hw, -h}, {0, 0}, {-hw, -h}};
            if (cap.arrow_filled && cap.arrow_middle_inset != 0) {
                src.points.push_back({0, -h + cap.arrow_middle_inset});
            }
            src.types.assign(src.points.size(), PathPointTypeLine);
            src.types[0] = PathPointTypeStart;
            if (cap.arrow_filled) src.types.back() |= PathPointTypeCloseSubpath;
            stroke = !cap.arrow_filled;
        } else if (cap.stroke_path) {
            src = *cap.stroke_path;
            stroke = true;
        } else if (cap.fill_path) {
            src = *cap.fill_path;
        } else {
            return;
        }
        const usize from = out.points.size();
        if (!stroke) {
            // A fill path is filled with the winding rule (Learn
            // CustomLineCap constructor); every figure is closed.
            for (usize i = 0; i < src.points.size(); ++i) {
                const D2 q = map(src.points[i]);
                out.points.push_back({static_cast<float>(q.x), static_cast<float>(q.y)});
                u8 t = static_cast<u8>(src.types[i] & (PathPointTypePathTypeMask |
                                                       PathPointTypeCloseSubpath));
                const bool last = i + 1 == src.points.size() ||
                                  (src.types[i + 1] & PathPointTypePathTypeMask) == 0;
                if (last) t |= PathPointTypeCloseSubpath;
                out.types.push_back(t);
            }
        } else {
            GpPath flat;
            flatten_path(src, nullptr, kFlatnessDefault, flat);
            GpPen sp;
            sp.width = static_cast<float>(w);
            sp.line_join = cap.stroke_join;
            sp.miter_limit = pen.miter_limit;
            sp.start_cap = cap.stroke_start_cap;
            sp.end_cap = cap.stroke_end_cap;
            Widener sw(sp, w, false, for_render);
            std::vector<Figure> figs = figures_of(flat);
            for (Figure& f : figs) {
                for (D2& q : f.pts) q = map({static_cast<float>(q.x), static_cast<float>(q.y)});
            }
            sw.run(figs, out);
        }
        if (for_render && path_area(out, from) < 0) reverse_figures(out, from);
    }

    void end_cap(GpPath& out, i32 cap, const GpCustomLineCap* custom, D2 c, D2 d) {
        if (cap == LineCapCustom && custom != nullptr) custom_cap(out, *custom, c, d);
        else if ((cap & LineCapAnchorMask) == 0x10) anchor(out, cap, c, d);
    }

    // -- figures -----------------------------------------------------------

    void open_figure(GpPath& out, std::vector<D2> p) {
        const std::vector<D2> d = dirs_of(p, false);
        const D2 s = p.front(), en = p.back();
        const D2 ds = d.front() * -1.0, de = d.back();
        const GpCustomLineCap* cs = pen.custom_start.get();
        const GpCustomLineCap* ce = pen.custom_end.get();
        const bool kept = trim(p, inset_of(pen.start_cap, cs), true) &&
                          trim(p, inset_of(pen.end_cap, ce), false);
        Emit e{out};
        if (kept) {
            const i32 sc = outline_cap(pen.start_cap), ec = outline_cap(pen.end_cap);
            if (!pen.dash_array.empty()) {
                const i32 dc = pen.dash_cap == DashCapRound || pen.dash_cap == DashCapTriangle
                                   ? pen.dash_cap
                                   : LineCapFlat;
                for (Dash& dsh : dash_poly(p, pen.dash_array, std::max(w, 1.0), pen.dash_offset,
                                           dc != LineCapFlat ? w : 0)) {
                    piece(e, dsh.pts, dsh.at_start ? sc : dc, dsh.at_end ? ec : dc);
                }
            } else {
                piece(e, p, sc, ec);
            }
        }
        end_cap(out, pen.start_cap, cs, s, ds);
        end_cap(out, pen.end_cap, ce, en, de);
    }

    void closed(GpPath& out, const std::vector<D2>& p) {
        Emit e{out};
        if (pen.dash_array.empty()) {
            if (p.size() >= 3 || (p.size() == 2 && pen.alignment != PenAlignmentInset)) {
                closed_figure(e, p);
            }
            return;
        }
        // A closed figure is dashed from its last point along the closing
        // segment first (probe closed-dash-w1/w2/w4, closed-dot-w2), with
        // dash caps at every dash end.
        std::vector<D2> q;
        q.push_back(p.back());
        q.insert(q.end(), p.begin(), p.end());
        const i32 dc = pen.dash_cap == DashCapRound || pen.dash_cap == DashCapTriangle
                           ? pen.dash_cap
                           : LineCapFlat;
        for (Dash& dsh : dash_poly(q, pen.dash_array, std::max(w, 1.0), pen.dash_offset,
                                   dc != LineCapFlat ? w : 0)) {
            piece(e, dsh.pts, dc, dc);
        }
    }

    void run(const std::vector<Figure>& figs, GpPath& out) {
        for (const Figure& f : figs) {
            if (f.pts.size() < 2) continue;  // a single point draws nothing
            if (f.closed) closed(out, f.pts);
            else open_figure(out, f.pts);
        }
    }
};

/// Physical pen units (Point, Inch, Document, Millimeter) widen to nothing
/// and draw nothing (probe line-unitpoint, r2 unit3..unit6).
bool unit_draws(i32 unit) { return unit == UnitWorld || unit == UnitPixel; }

void transform_path(GpPath& p, const Lin& m) {
    for (PointF& q : p.points) {
        const D2 r = m.apply({q.X, q.Y});
        q = {static_cast<float>(r.x), static_cast<float>(r.y)};
    }
}

}  // namespace

void widen_path(const GpPath& path, const GpPen& pen, const Affine* m, float flatness,
                GpPath& out) {
    out = GpPath{};
    out.fill_mode = FillModeWinding;  // probe: every widened path reads fill=1
    if (!unit_draws(pen.unit)) return;
    GpPath flat;
    flatten_path(path, m, flatness, flat);
    double w = std::fabs(pen.width);
    Lin nib = lin_of(pen.transform);
    if (w == 0) {  // probe line-w0: one unit wide, pen transform ignored
        w = 1;
        nib = {};
    }
    if (std::fabs(nib.det()) < 1e-12) return;
    transform_path(flat, nib.inverse());
    Widener wd(pen, w, false, false);
    wd.run(figures_of(flat), out);
    transform_path(out, nib);
    out.new_figure = true;
}

Status stroke_path(RenderScope& scope, const GpPath& world_path, const GpPen& pen) {
    if (!pen.brush) return Status::InvalidParameter;
    if (scope.status() != Status::Ok || scope.cg() == nullptr) return scope.status();
    if (!unit_draws(pen.unit)) return Status::Ok;
    const GpGraphics& g = scope.graphics();
    const Affine wd = world_to_device(g);
    const Lin wlin = lin_of(wd);
    if (std::fabs(wlin.det()) < 1e-12) return Status::Ok;
    GpPath flat;
    flatten_path(world_path, &wd, kFlatnessDefault, flat);
    if (flat.points.empty()) return Status::Ok;

    const Lin pen_lin = lin_of(pen.transform);
    Lin nib = pen_lin;
    if (pen.unit == UnitWorld) {
        const Affine a{static_cast<float>(pen_lin.m11), static_cast<float>(pen_lin.m12),
                       static_cast<float>(pen_lin.m21), static_cast<float>(pen_lin.m22), 0, 0};
        nib = lin_of(a.then(wd));
    }
    double w = std::fabs(pen.width);
    // A pen at most one device pixel wide (width 0 included) draws a
    // nominal one-pixel line; aliased, it covers both end pixels (probe
    // line-w0*, line-w0.3-*, line-w1-y20-ends).
    const i32 sm = g.state.smoothing_mode;
    const bool aa = sm == 2 || sm == 4 || sm == 5;
    bool nominal = w == 0 || w * nib.stretch() <= 1.0 || std::fabs(nib.det()) < 1e-12;
    if (nominal) {
        w = 1;
        nib = {};
    }
    transform_path(flat, nib.inverse());
    GpPath outline;
    Widener wdn(pen, w, nominal && !aa, true);
    wdn.run(figures_of(flat), outline);
    if (outline.points.empty()) return Status::Ok;
    transform_path(outline, nib);
    // Back to world space for the brush (fill_path takes world coordinates).
    const Lin inv = wlin.inverse();
    for (PointF& q : outline.points) {
        const D2 r = inv.apply({q.X - wd.dx, q.Y - wd.dy});
        q = {static_cast<float>(r.x), static_cast<float>(r.y)};
    }
    outline.fill_mode = FillModeWinding;
    CGMutablePathRef cg = path_to_cg(outline);
    if (cg == nullptr) return Status::OutOfMemory;
    const Status s = paint_path(scope, cg, false, *pen.brush);
    CGPathRelease(cg);
    return s;
}

namespace {
Status paint_centred(RenderScope& scope, CGContextRef cg, const CGAffineTransform& ctm,
                     CGPathRef path, bool even_odd, const GpBrush& brush);
}  // namespace

Status paint_path(RenderScope& scope, CGPathRef path, bool even_odd, const GpBrush& brush) {
    CGContextRef cg = scope.cg();
    if (cg == nullptr || path == nullptr) return scope.status();
    return paint_centred(scope, cg, CGContextGetCTM(cg), path, even_odd, brush);
}

namespace {

Status paint_centred(RenderScope& scope, CGContextRef cg, const CGAffineTransform& ctm,
                     CGPathRef path, bool even_odd, const GpBrush& brush) {
    const i32 sm = scope.graphics().state.smoothing_mode;
    if (sm == 2 || sm == 4 || sm == 5) return fill_path(scope, path, even_odd, brush);
    const i32 w = scope.width(), h = scope.height();
    // Antialiased coverage mask: pixels fully in or out are decided by it;
    // every partly covered pixel is decided by testing the point just
    // below-right of its centre (centre + 1/256 px), which puts a centre on
    // a left/top edge inside and one on a right/bottom edge outside.
    std::vector<u8> mask(static_cast<usize>(w) * static_cast<usize>(h), 0);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef mc = CGBitmapContextCreate(mask.data(), static_cast<size_t>(w),
                                            static_cast<size_t>(h), 8, static_cast<size_t>(w),
                                            gray, static_cast<u32>(kCGImageAlphaNone));
    if (mc == nullptr) { CGColorSpaceRelease(gray); return Status::OutOfMemory; }
    CGContextConcatCTM(mc, ctm);
    CGContextSetShouldAntialias(mc, true);
    CGContextSetGrayFillColor(mc, 1.0, 1.0);
    CGContextAddPath(mc, path);
    if (even_odd) CGContextEOFillPath(mc); else CGContextFillPath(mc);
    CGContextFlush(mc);
    CGContextRelease(mc);
    constexpr double kNudge = 1.0 / 256.0;
    const CGAffineTransform inv_ctm = CGAffineTransformInvert(ctm);
    bool any = false;
    for (i32 row = 0; row < h; ++row) {
        for (i32 x = 0; x < w; ++x) {
            u8& v = mask[static_cast<usize>(row) * static_cast<usize>(w) + static_cast<usize>(x)];
            if (v > 0 && v < 255) {
                // Canvas row 0 is the top; CoreGraphics base space is y-up.
                const CGPoint pt = CGPointApplyAffineTransform(
                    CGPointMake(x + 0.5 + kNudge, (h - row) - 0.5 - kNudge), inv_ctm);
                v = CGPathContainsPoint(path, nullptr, pt, even_odd) ? 0xFF : 0x00;
            }
            any |= v != 0;
        }
    }
    if (!any) { CGColorSpaceRelease(gray); return Status::Ok; }
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, mask.data(), mask.size(), nullptr);
    CGImageRef img = CGImageCreate(static_cast<size_t>(w), static_cast<size_t>(h), 8, 8,
                                   static_cast<size_t>(w), gray, static_cast<u32>(kCGImageAlphaNone),
                                   dp, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(gray);
    if (img == nullptr) return Status::OutOfMemory;
    CGContextSaveGState(cg);
    CGContextConcatCTM(cg, CGAffineTransformInvert(ctm));
    CGContextClipToMask(cg, CGRectMake(0, 0, w, h), img);
    CGContextConcatCTM(cg, ctm);
    // Paint the whole canvas through the mask (the brush keeps its world
    // placement).
    CGMutablePathRef all = CGPathCreateMutable();
    const CGAffineTransform inv = CGAffineTransformInvert(ctm);
    CGPathAddRect(all, &inv, CGRectMake(0, 0, w, h));
    const Status s = fill_path(scope, all, false, brush);
    CGPathRelease(all);
    CGContextRestoreGState(cg);
    CGImageRelease(img);
    return s;
}

}  // namespace

// ---- Exports ----------------------------------------------------------------

namespace {

void reg(HandlerMap& map, const char* name, std::function<Status(Marshaler&)> fn) {
    map[name] = [fn = std::move(fn)](Marshaler& m) { write_status(m, fn(m)); };
}

bool valid_pen_unit(i32 u) { return u == UnitWorld || (u >= UnitPixel && u <= UnitMillimeter); }

/// DashCap values other than Round/Triangle are stored as Flat (probe setdashcap).
i32 dash_cap_of(i32 v) { return (v == DashCapRound || v == DashCapTriangle) ? v : DashCapFlat; }

bool singular(const Affine& a) { return a.m11 * a.m22 - a.m12 * a.m21 == 0; }

/// Base and stroke caps of a custom cap: Flat/Square/Round/Triangle only
/// (probe r2 setstrokecaps / setbasecap).
bool simple_cap(i32 c) { return c >= LineCapFlat && c <= LineCapTriangle; }

/// GdipCreateCustomLineCap: a fill path must cross the negative y axis,
/// where the line meets the cap (probe createcap fill / r2 fill-*).
bool crosses_negative_y(const GpPath& path) {
    GpPath flat;
    flatten_path(path, nullptr, kFlatnessDefault, flat);
    for (const Figure& f : figures_of(flat)) {
        const usize n = f.pts.size();
        for (usize i = 0; i < n; ++i) {
            const D2 a = f.pts[i], b = f.pts[(i + 1) % n];
            if (a.x == 0 && a.y < 0) return true;
            if ((a.x < 0) != (b.x < 0) && b.x != a.x) {
                const double y = a.y + (b.y - a.y) * (0 - a.x) / (b.x - a.x);
                if (y < 0) return true;
            }
        }
    }
    return false;
}

void arrow_update_inset(GpCustomLineCap& c) {
    // The line ends where the arrow is one pen width wide: height / width
    // (probe arrow inset 1.5 for 3x2, 1.25 for 5x4; 0 for a zero side).
    c.base_inset = (c.arrow_width != 0 && c.arrow_height != 0)
                       ? c.arrow_height / c.arrow_width
                       : 0.0f;
}

}  // namespace

void register_pen(GdiplusManager& mgr, HandlerMap& map) {
    ObjectTable& objs = mgr.objects();
    auto pen_of = [&mgr](Marshaler& m, usize i, GpPen*& out) {
        const u64 h = handle_arg(m, i);
        out = mgr.objects().get<GpPen>(h);
        return resolve_status(mgr, h, out != nullptr);
    };
    auto cap_of = [&mgr](Marshaler& m, usize i, GpCustomLineCap*& out) {
        const u64 h = handle_arg(m, i);
        out = mgr.objects().get<GpCustomLineCap>(h);
        return resolve_status(mgr, h, out != nullptr);
    };
    auto arrow_of = [cap_of](Marshaler& m, usize i, GpCustomLineCap*& out) {
        Status s = cap_of(m, i, out);
        if (s == Status::Ok && out->type != CustomLineCapTypeAdjustableArrow) {
            s = Status::InvalidParameter;
        }
        return s;
    };

    // GdipCreatePen1(ARGB, REAL width, GpUnit, GpPen**)
    reg(map, "GdipCreatePen1", [&objs](Marshaler& m) {
        const u32 argb = m.arg32(0);
        const float width = real_arg(m, 1);
        const i32 unit = static_cast<i32>(m.arg32(2));
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0 || !valid_pen_unit(unit)) return Status::InvalidParameter;
        GpPen pen;
        pen.width = std::fabs(width);  // probe create1 width=-1 -> 1
        pen.unit = unit;
        pen.brush = std::make_shared<GpBrush>();
        pen.brush->type = BrushTypeSolidColor;
        pen.brush->color = argb;
        const u64 h = objs.add(std::move(pen));
        if (!write_handle(m, out, h)) { objs.remove<GpPen>(h); return Status::InvalidParameter; }
        return Status::Ok;
    });
    // GdipCreatePen2(GpBrush*, REAL width, GpUnit, GpPen**)
    reg(map, "GdipCreatePen2", [&mgr, &objs](Marshaler& m) {
        const u64 hb = handle_arg(m, 0);
        GpBrush* brush = objs.get<GpBrush>(hb);
        const Status bs = resolve_status(mgr, hb, brush != nullptr);
        if (bs != Status::Ok) return bs;
        const float width = real_arg(m, 1);
        const i32 unit = static_cast<i32>(m.arg32(2));
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0 || !valid_pen_unit(unit)) return Status::InvalidParameter;
        GpPen pen;
        pen.width = std::fabs(width);
        pen.unit = unit;
        pen.brush = std::make_shared<GpBrush>(*brush);  // the pen keeps its own copy
        const u64 h = objs.add(std::move(pen));
        if (!write_handle(m, out, h)) { objs.remove<GpPen>(h); return Status::InvalidParameter; }
        return Status::Ok;
    });
    reg(map, "GdipClonePen", [pen_of, &objs](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return Status::InvalidParameter;
        const u64 h = objs.add(clone_pen(*p));
        if (!write_handle(m, out, h)) { objs.remove<GpPen>(h); return Status::InvalidParameter; }
        return Status::Ok;
    });
    reg(map, "GdipDeletePen", [pen_of, &objs](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        objs.remove<GpPen>(handle_arg(m, 0));
        return Status::Ok;
    });

    // ---- REAL / INT state with a plain getter -----------------------------
    auto get_real = [pen_of](const char* name, HandlerMap& mp, float GpPen::*field) {
        reg(mp, name, [pen_of, field](Marshaler& m) {
            GpPen* p = nullptr;
            if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return Status::InvalidParameter;
            return write_f32(m, out, p->*field) ? Status::Ok : Status::InvalidParameter;
        });
    };
    auto get_int = [pen_of](const char* name, HandlerMap& mp, i32 GpPen::*field) {
        reg(mp, name, [pen_of, field](Marshaler& m) {
            GpPen* p = nullptr;
            if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return Status::InvalidParameter;
            return m.write_u32(out, static_cast<u32>(p->*field)).is_ok() ? Status::Ok
                                                                          : Status::InvalidParameter;
        });
    };
    get_real("GdipGetPenWidth", map, &GpPen::width);
    get_real("GdipGetPenMiterLimit", map, &GpPen::miter_limit);
    get_real("GdipGetPenDashOffset", map, &GpPen::dash_offset);
    get_int("GdipGetPenUnit", map, &GpPen::unit);
    get_int("GdipGetPenStartCap", map, &GpPen::start_cap);
    get_int("GdipGetPenEndCap", map, &GpPen::end_cap);
    get_int("GdipGetPenDashCap197819", map, &GpPen::dash_cap);
    get_int("GdipGetPenLineJoin", map, &GpPen::line_join);
    get_int("GdipGetPenMode", map, &GpPen::alignment);
    get_int("GdipGetPenDashStyle", map, &GpPen::dash_style);

    reg(map, "GdipSetPenWidth", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        p->width = real_arg(m, 1);  // stored as given (probe setwidth -1)
        return Status::Ok;
    });
    reg(map, "GdipSetPenUnit", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const i32 u = static_cast<i32>(m.arg32(1));
        if (!valid_pen_unit(u)) return Status::InvalidParameter;
        p->unit = u;
        return Status::Ok;
    });
    // Line caps are stored as given, any value (probe setcap); only a
    // custom cap object makes LineCapCustom draw a custom cap.
    auto set_start = [](GpPen& p, i32 cap) {
        p.start_cap = cap;
        if (cap != LineCapCustom) p.custom_start.reset();
    };
    auto set_end = [](GpPen& p, i32 cap) {
        p.end_cap = cap;
        if (cap != LineCapCustom) p.custom_end.reset();
    };
    reg(map, "GdipSetPenLineCap197819", [pen_of, set_start, set_end](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        set_start(*p, static_cast<i32>(m.arg32(1)));
        set_end(*p, static_cast<i32>(m.arg32(2)));
        p->dash_cap = dash_cap_of(static_cast<i32>(m.arg32(3)));
        return Status::Ok;
    });
    reg(map, "GdipSetPenStartCap", [pen_of, set_start](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        set_start(*p, static_cast<i32>(m.arg32(1)));
        return Status::Ok;
    });
    reg(map, "GdipSetPenEndCap", [pen_of, set_end](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        set_end(*p, static_cast<i32>(m.arg32(1)));
        return Status::Ok;
    });
    reg(map, "GdipSetPenDashCap197819", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        p->dash_cap = dash_cap_of(static_cast<i32>(m.arg32(1)));
        return Status::Ok;
    });
    reg(map, "GdipSetPenLineJoin", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        p->line_join = static_cast<i32>(m.arg32(1));  // any value (probe setjoin)
        return Status::Ok;
    });
    reg(map, "GdipSetPenMiterLimit", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        p->miter_limit = std::max(1.0f, real_arg(m, 1));  // probe setmiter: clamped to 1
        return Status::Ok;
    });
    reg(map, "GdipSetPenMode", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        p->alignment = static_cast<i32>(m.arg32(1));  // any value (probe setmode)
        return Status::Ok;
    });

    // ---- Custom caps on a pen ---------------------------------------------
    auto set_custom = [pen_of, cap_of](bool start) {
        return [pen_of, cap_of, start](Marshaler& m) {
            GpPen* p = nullptr;
            if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
            GpCustomLineCap* c = nullptr;
            if (Status s = cap_of(m, 1, c); s != Status::Ok) return s;
            if (start) { p->custom_start = clone_cap(*c); p->start_cap = LineCapCustom; }
            else { p->custom_end = clone_cap(*c); p->end_cap = LineCapCustom; }
            return Status::Ok;
        };
    };
    auto get_custom = [pen_of, &objs](bool start) {
        return [pen_of, &objs, start](Marshaler& m) {
            GpPen* p = nullptr;
            if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return Status::InvalidParameter;
            const auto& c = start ? p->custom_start : p->custom_end;
            // No custom cap: Ok and a NULL cap (probe getcustomstart none).
            const u64 h = c ? objs.add(clone_cap(*c)) : 0;
            if (!write_handle(m, out, h)) {
                if (h) objs.remove<GpCustomLineCap>(h);
                return Status::InvalidParameter;
            }
            return Status::Ok;
        };
    };
    reg(map, "GdipSetPenCustomStartCap", set_custom(true));
    reg(map, "GdipSetPenCustomEndCap", set_custom(false));
    reg(map, "GdipGetPenCustomStartCap", get_custom(true));
    reg(map, "GdipGetPenCustomEndCap", get_custom(false));

    // ---- Transform --------------------------------------------------------
    auto matrix_of = [&mgr](Marshaler& m, usize i, GpMatrix*& out) {
        const u64 h = handle_arg(m, i);
        out = mgr.objects().get<GpMatrix>(h);
        return resolve_status(mgr, h, out != nullptr);
    };
    reg(map, "GdipSetPenTransform", [pen_of, matrix_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        GpMatrix* mx = nullptr;
        if (Status s = matrix_of(m, 1, mx); s != Status::Ok) return s;
        if (singular(mx->m)) return Status::InvalidParameter;
        p->transform = mx->m;
        return Status::Ok;
    });
    reg(map, "GdipGetPenTransform", [pen_of, matrix_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        GpMatrix* mx = nullptr;
        if (Status s = matrix_of(m, 1, mx); s != Status::Ok) return s;
        mx->m = p->transform;
        return Status::Ok;
    });
    reg(map, "GdipResetPenTransform", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        p->transform = Affine{};
        return Status::Ok;
    });
    // MatrixOrderPrepend (0): the new matrix applies first; Append (1): last.
    auto combine = [](GpPen& p, const Affine& a, i32 order) {
        if (order == 0) p.transform = a.then(p.transform);
        else if (order == 1) p.transform = p.transform.then(a);
        else return Status::InvalidParameter;
        return Status::Ok;
    };
    reg(map, "GdipMultiplyPenTransform", [&mgr, pen_of, combine](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const u64 hm = handle_arg(m, 1);
        const i32 order = static_cast<i32>(m.arg32(2));
        if (hm == 0) return Status::Ok;  // probe: multiply NULL -> Ok, unchanged
        GpMatrix* mx = mgr.objects().get<GpMatrix>(hm);
        if (Status s = resolve_status(mgr, hm, mx != nullptr); s != Status::Ok) return s;
        if (singular(mx->m)) return Status::InvalidParameter;
        return combine(*p, mx->m, order);
    });
    reg(map, "GdipTranslatePenTransform", [pen_of, combine](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const float dx = real_arg(m, 1), dy = real_arg(m, 2);
        return combine(*p, Affine{1, 0, 0, 1, dx, dy}, static_cast<i32>(m.arg32(3)));
    });
    reg(map, "GdipScalePenTransform", [pen_of, combine](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const float sx = real_arg(m, 1), sy = real_arg(m, 2);
        return combine(*p, Affine{sx, 0, 0, sy, 0, 0}, static_cast<i32>(m.arg32(3)));
    });
    reg(map, "GdipRotatePenTransform", [pen_of, combine](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const double a = static_cast<double>(real_arg(m, 1)) * kPi / 180.0;
        const float c = static_cast<float>(std::cos(a)), sn = static_cast<float>(std::sin(a));
        return combine(*p, Affine{c, sn, -sn, c, 0, 0}, static_cast<i32>(m.arg32(2)));
    });

    // ---- Fill -------------------------------------------------------------
    reg(map, "GdipSetPenColor", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        // Any pen becomes a solid pen of this colour (probe setcolor on hatch).
        auto b = std::make_shared<GpBrush>();
        b->type = BrushTypeSolidColor;
        b->color = m.arg32(1);
        p->brush = std::move(b);
        return Status::Ok;
    });
    reg(map, "GdipGetPenColor", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return Status::InvalidParameter;
        u32 argb = 0;
        const bool solid = p->brush && brush_solid_argb(*p->brush, argb);
        // A non-solid pen: InvalidParameter, and the output is overwritten
        // (probe: 0 on x64, stack residue on x86).
        if (!m.write_u32(out, solid ? argb : 0).is_ok()) return Status::InvalidParameter;
        return solid ? Status::Ok : Status::InvalidParameter;
    });
    reg(map, "GdipSetPenBrushFill", [&mgr, pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const u64 hb = handle_arg(m, 1);
        GpBrush* b = mgr.objects().get<GpBrush>(hb);
        if (Status s = resolve_status(mgr, hb, b != nullptr); s != Status::Ok) return s;
        p->brush = std::make_shared<GpBrush>(*b);
        return Status::Ok;
    });
    reg(map, "GdipGetPenBrushFill", [pen_of, &objs](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || !p->brush) return Status::InvalidParameter;
        // A new brush object the caller deletes (probe getbrushfill same=0).
        const u64 h = objs.add(std::make_shared<GpBrush>(*p->brush));
        if (!write_handle(m, out, h)) { objs.remove<GpBrush>(h); return Status::InvalidParameter; }
        return Status::Ok;
    });
    reg(map, "GdipGetPenFillType", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return Status::InvalidParameter;
        const i32 t = p->brush ? p->brush->type : -1;  // PenTypeUnknown
        return m.write_u32(out, static_cast<u32>(t)).is_ok() ? Status::Ok
                                                              : Status::InvalidParameter;
    });

    // ---- Dashes -----------------------------------------------------------
    reg(map, "GdipSetPenDashStyle", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const i32 style = static_cast<i32>(m.arg32(1));
        // Out-of-range styles: Ok, unchanged (probe setdashstyle -1/6, r2).
        // Custom keeps the current array; Solid clears it.
        if (style < DashStyleSolid || style > DashStyleCustom) return Status::Ok;
        if (style != DashStyleCustom) p->dash_array = dash_pattern(style);
        p->dash_style = style;
        return Status::Ok;
    });
    reg(map, "GdipSetPenDashOffset", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        p->dash_offset = real_arg(m, 1);
        return Status::Ok;
    });
    auto get_count = [pen_of](std::vector<float> GpPen::*field) {
        return [pen_of, field](Marshaler& m) {
            GpPen* p = nullptr;
            if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return Status::InvalidParameter;
            return m.write_u32(out, static_cast<u32>((p->*field).size())).is_ok()
                       ? Status::Ok
                       : Status::InvalidParameter;
        };
    };
    // Get*Array(pen, REAL*, count): count may be below the stored size
    // (that many copied); above it, or negative, is InvalidParameter.
    auto get_array = [pen_of](std::vector<float> GpPen::*field) {
        return [pen_of, field](Marshaler& m) {
            GpPen* p = nullptr;
            if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
            const GuestAddr out = m.arg_ptr(1);
            const i32 count = static_cast<i32>(m.arg32(2));
            const std::vector<float>& v = p->*field;
            if (out == 0 || count < 0 || static_cast<usize>(count) > v.size()) {
                return Status::InvalidParameter;
            }
            for (i32 i = 0; i < count; ++i) {
                if (!write_f32(m, out + static_cast<u64>(i) * 4, v[static_cast<usize>(i)])) {
                    return Status::InvalidParameter;
                }
            }
            return Status::Ok;
        };
    };
    reg(map, "GdipGetPenDashCount", get_count(&GpPen::dash_array));
    reg(map, "GdipGetPenDashArray", get_array(&GpPen::dash_array));
    reg(map, "GdipGetPenCompoundCount", get_count(&GpPen::compound));
    reg(map, "GdipGetPenCompoundArray", get_array(&GpPen::compound));
    reg(map, "GdipSetPenDashArray", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const GuestAddr at = m.arg_ptr(1);
        const i32 count = static_cast<i32>(m.arg32(2));
        if (at == 0 || count <= 0) return Status::InvalidParameter;
        std::vector<float> v(static_cast<usize>(count));
        for (i32 i = 0; i < count; ++i) {
            if (!read_f32(m, at + static_cast<u64>(i) * 4, v[static_cast<usize>(i)]) ||
                !(v[static_cast<usize>(i)] > 0)) {
                return Status::InvalidParameter;  // probe: zero / negative entries
            }
        }
        p->dash_array = std::move(v);
        p->dash_style = DashStyleCustom;
        return Status::Ok;
    });
    reg(map, "GdipSetPenCompoundArray", [pen_of](Marshaler& m) {
        GpPen* p = nullptr;
        if (Status s = pen_of(m, 0, p); s != Status::Ok) return s;
        const GuestAddr at = m.arg_ptr(1);
        const i32 count = static_cast<i32>(m.arg32(2));
        // An even count >= 2 of non-decreasing values in [0, 1] (probe setcompound*).
        if (at == 0 || count < 2 || (count % 2) != 0) return Status::InvalidParameter;
        std::vector<float> v(static_cast<usize>(count));
        for (i32 i = 0; i < count; ++i) {
            float f = 0;
            if (!read_f32(m, at + static_cast<u64>(i) * 4, f)) return Status::InvalidParameter;
            if (!(f >= 0 && f <= 1) || (i > 0 && f < v[static_cast<usize>(i - 1)])) {
                return Status::InvalidParameter;
            }
            v[static_cast<usize>(i)] = f;
        }
        p->compound = std::move(v);
        return Status::Ok;
    });

    // ---- CustomLineCap ----------------------------------------------------
    // GdipCreateCustomLineCap(GpPath* fill, GpPath* stroke, GpLineCap base, REAL inset, GpCustomLineCap**)
    reg(map, "GdipCreateCustomLineCap", [&mgr, &objs](Marshaler& m) {
        const u64 hf = handle_arg(m, 0), hs = handle_arg(m, 1);
        const i32 base = static_cast<i32>(m.arg32(2));
        const float inset = real_arg(m, 3);
        const GuestAddr out = m.arg_ptr(4);
        if (out == 0 || (hf == 0 && hs == 0)) return Status::InvalidParameter;
        GpPath* fill = hf ? objs.get<GpPath>(hf) : nullptr;
        GpPath* stroke = hs ? objs.get<GpPath>(hs) : nullptr;
        if (hf && fill == nullptr) return resolve_status(mgr, hf, false);
        if (hs && stroke == nullptr) return resolve_status(mgr, hs, false);
        if (!simple_cap(base)) return Status::InvalidParameter;
        if (fill != nullptr && !crosses_negative_y(*fill)) {
            log_unimplemented("GdipCreateCustomLineCap",
                              "fill path not crossing the negative y axis (GDI+ refuses it too)");
            return Status::NotImplemented;
        }
        auto cap = std::make_shared<GpCustomLineCap>();
        // Both given: the fill path is ignored (Learn CustomLineCap constructor).
        if (stroke != nullptr) cap->stroke_path = std::make_shared<GpPath>(*stroke);
        else cap->fill_path = std::make_shared<GpPath>(*fill);
        cap->base_cap = base;
        cap->base_inset = inset;
        const u64 h = objs.add(std::move(cap));
        if (!write_handle(m, out, h)) {
            objs.remove<GpCustomLineCap>(h);
            return Status::InvalidParameter;
        }
        return Status::Ok;
    });
    reg(map, "GdipDeleteCustomLineCap", [cap_of, &objs](Marshaler& m) {
        GpCustomLineCap* c = nullptr;
        if (Status s = cap_of(m, 0, c); s != Status::Ok) return s;
        objs.remove<GpCustomLineCap>(handle_arg(m, 0));
        return Status::Ok;
    });
    reg(map, "GdipCloneCustomLineCap", [cap_of, &objs](Marshaler& m) {
        GpCustomLineCap* c = nullptr;
        if (Status s = cap_of(m, 0, c); s != Status::Ok) return s;
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return Status::InvalidParameter;
        const u64 h = objs.add(clone_cap(*c));
        if (!write_handle(m, out, h)) {
            objs.remove<GpCustomLineCap>(h);
            return Status::InvalidParameter;
        }
        return Status::Ok;
    });
    auto cap_get_int = [cap_of](i32 GpCustomLineCap::*field) {
        return [cap_of, field](Marshaler& m) {
            GpCustomLineCap* c = nullptr;
            if (Status s = cap_of(m, 0, c); s != Status::Ok) return s;
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return Status::InvalidParameter;
            return m.write_u32(out, static_cast<u32>(c->*field)).is_ok() ? Status::Ok
                                                                          : Status::InvalidParameter;
        };
    };
    auto cap_get_real = [cap_of](float GpCustomLineCap::*field) {
        return [cap_of, field](Marshaler& m) {
            GpCustomLineCap* c = nullptr;
            if (Status s = cap_of(m, 0, c); s != Status::Ok) return s;
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return Status::InvalidParameter;
            return write_f32(m, out, c->*field) ? Status::Ok : Status::InvalidParameter;
        };
    };
    reg(map, "GdipGetCustomLineCapType", cap_get_int(&GpCustomLineCap::type));
    reg(map, "GdipGetCustomLineCapStrokeJoin", cap_get_int(&GpCustomLineCap::stroke_join));
    reg(map, "GdipGetCustomLineCapBaseCap", cap_get_int(&GpCustomLineCap::base_cap));
    reg(map, "GdipGetCustomLineCapBaseInset", cap_get_real(&GpCustomLineCap::base_inset));
    reg(map, "GdipGetCustomLineCapWidthScale", cap_get_real(&GpCustomLineCap::width_scale));
    reg(map, "GdipSetCustomLineCapStrokeCaps", [cap_of](Marshaler& m) {
        GpCustomLineCap* c = nullptr;
        if (Status s = cap_of(m, 0, c); s != Status::Ok) return s;
        const i32 a = static_cast<i32>(m.arg32(1)), b = static_cast<i32>(m.arg32(2));
        if (!simple_cap(a) || !simple_cap(b)) return Status::InvalidParameter;
        c->stroke_start_cap = a;
        c->stroke_end_cap = b;
        return Status::Ok;
    });
    reg(map, "GdipGetCustomLineCapStrokeCaps", [cap_of](Marshaler& m) {
        GpCustomLineCap* c = nullptr;
        if (Status s = cap_of(m, 0, c); s != Status::Ok) return s;
        const GuestAddr a = m.arg_ptr(1), b = m.arg_ptr(2);
        if (a == 0 || b == 0) return Status::InvalidParameter;
        const bool ok = m.write_u32(a, static_cast<u32>(c->stroke_start_cap)).is_ok() &&
                        m.write_u32(b, static_cast<u32>(c->stroke_end_cap)).is_ok();
        return ok ? Status::Ok : Status::InvalidParameter;
    });
    reg(map, "GdipSetCustomLineCapStrokeJoin", [cap_of](Marshaler& m) {
        GpCustomLineCap* c = nullptr;
        if (Status s = cap_of(m, 0, c); s != Status::Ok) return s;
        c->stroke_join = static_cast<i32>(m.arg32(1));  // any value (probe r2 setstrokejoin 4)
        return Status::Ok;
    });
    reg(map, "GdipSetCustomLineCapBaseCap", [cap_of](Marshaler& m) {
        GpCustomLineCap* c = nullptr;
        if (Status s = cap_of(m, 0, c); s != Status::Ok) return s;
        const i32 b = static_cast<i32>(m.arg32(1));
        if (!simple_cap(b)) return Status::InvalidParameter;
        c->base_cap = b;
        return Status::Ok;
    });
    reg(map, "GdipSetCustomLineCapBaseInset", [cap_of](Marshaler& m) {
        GpCustomLineCap* c = nullptr;
        if (Status s = cap_of(m, 0, c); s != Status::Ok) return s;
        c->base_inset = real_arg(m, 1);
        return Status::Ok;
    });
    reg(map, "GdipSetCustomLineCapWidthScale", [cap_of](Marshaler& m) {
        GpCustomLineCap* c = nullptr;
        if (Status s = cap_of(m, 0, c); s != Status::Ok) return s;
        c->width_scale = real_arg(m, 1);
        return Status::Ok;
    });

    // ---- AdjustableArrowCap -----------------------------------------------
    // GdipCreateAdjustableArrowCap(REAL height, REAL width, BOOL isFilled, GpAdjustableArrowCap**)
    reg(map, "GdipCreateAdjustableArrowCap", [&objs](Marshaler& m) {
        const float h = real_arg(m, 0), w = real_arg(m, 1);
        const bool filled = m.arg32(2) != 0;
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0) return Status::InvalidParameter;
        auto cap = std::make_shared<GpCustomLineCap>();
        cap->type = CustomLineCapTypeAdjustableArrow;
        cap->arrow_height = h;
        cap->arrow_width = w;
        cap->arrow_filled = filled;
        cap->base_cap = LineCapTriangle;  // probe arrow base=3
        arrow_update_inset(*cap);
        const u64 hh = objs.add(std::move(cap));
        if (!write_handle(m, out, hh)) {
            objs.remove<GpCustomLineCap>(hh);
            return Status::InvalidParameter;
        }
        return Status::Ok;
    });
    auto arrow_set = [arrow_of](float GpCustomLineCap::*field, bool inset) {
        return [arrow_of, field, inset](Marshaler& m) {
            GpCustomLineCap* c = nullptr;
            if (Status s = arrow_of(m, 0, c); s != Status::Ok) return s;
            c->*field = real_arg(m, 1);
            if (inset) arrow_update_inset(*c);
            return Status::Ok;
        };
    };
    auto arrow_get = [arrow_of](float GpCustomLineCap::*field) {
        return [arrow_of, field](Marshaler& m) {
            GpCustomLineCap* c = nullptr;
            if (Status s = arrow_of(m, 0, c); s != Status::Ok) return s;
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return Status::InvalidParameter;
            return write_f32(m, out, c->*field) ? Status::Ok : Status::InvalidParameter;
        };
    };
    reg(map, "GdipSetAdjustableArrowCapHeight", arrow_set(&GpCustomLineCap::arrow_height, true));
    reg(map, "GdipSetAdjustableArrowCapWidth", arrow_set(&GpCustomLineCap::arrow_width, true));
    reg(map, "GdipSetAdjustableArrowCapMiddleInset",
        arrow_set(&GpCustomLineCap::arrow_middle_inset, false));
    reg(map, "GdipGetAdjustableArrowCapHeight", arrow_get(&GpCustomLineCap::arrow_height));
    reg(map, "GdipGetAdjustableArrowCapWidth", arrow_get(&GpCustomLineCap::arrow_width));
    reg(map, "GdipGetAdjustableArrowCapMiddleInset",
        arrow_get(&GpCustomLineCap::arrow_middle_inset));
    reg(map, "GdipSetAdjustableArrowCapFillState", [arrow_of](Marshaler& m) {
        GpCustomLineCap* c = nullptr;
        if (Status s = arrow_of(m, 0, c); s != Status::Ok) return s;
        c->arrow_filled = m.arg32(1) != 0;
        return Status::Ok;
    });
    reg(map, "GdipGetAdjustableArrowCapFillState", [arrow_of](Marshaler& m) {
        GpCustomLineCap* c = nullptr;
        if (Status s = arrow_of(m, 0, c); s != Status::Ok) return s;
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return Status::InvalidParameter;
        return m.write_u32(out, c->arrow_filled ? 1u : 0u).is_ok() ? Status::Ok
                                                                    : Status::InvalidParameter;
    });
}

}  // namespace vitreo::winapi::gdip
