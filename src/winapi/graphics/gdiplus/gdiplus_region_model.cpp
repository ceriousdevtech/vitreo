// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_region_model.cpp
/// @brief GpRegion: combine/transform rules, the device-pixel rasterizer,
/// GetRegionData serialization, and the CoreGraphics clip (see
/// gdiplus_region.hpp; rules from gdiplus_region_probe.expected).

#include "winapi/graphics/gdiplus/gdiplus_region.hpp"
#include "winapi/graphics/gdiplus/gdiplus_render.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vitreo::winapi::gdip {

// ---- RegionScans -----------------------------------------------------------

namespace {

bool span_rule(i32 mode, bool a, bool b) {
    switch (mode) {
    case CombineModeIntersect: return a && b;
    case CombineModeUnion: return a || b;
    case CombineModeXor: return a != b;
    case CombineModeExclude: return a && !b;
    case CombineModeComplement: return b && !a;
    default: return b;  // Replace
    }
}

/// 1-D combine of two canonical span lists.
std::vector<i32> span_op(const std::vector<i32>& a, const std::vector<i32>& b, i32 mode) {
    std::vector<i32> out;
    usize i = 0, j = 0;
    bool in_a = false, in_b = false, in_r = false;
    while (i < a.size() || j < b.size()) {
        const i32 x = (j >= b.size() || (i < a.size() && a[i] <= b[j])) ? a[i] : b[j];
        while (i < a.size() && a[i] == x) { in_a = !in_a; ++i; }
        while (j < b.size() && b[j] == x) { in_b = !in_b; ++j; }
        const bool r = span_rule(mode, in_a, in_b);
        if (r != in_r) { out.push_back(x); in_r = r; }
    }
    return out;
}

/// Append rows [y0, y1) with @p xs, coalescing with an identical band above.
void append_band(std::vector<ScanBand>& bands, i32 y0, i32 y1, std::vector<i32> xs) {
    if (xs.empty() || y1 <= y0) return;
    if (!bands.empty() && bands.back().y1 == y0 && bands.back().xs == xs) {
        bands.back().y1 = y1;
        return;
    }
    bands.push_back(ScanBand{y0, y1, std::move(xs)});
}

}  // namespace

RegionScans RegionScans::rect(i32 x0, i32 y0, i32 x1, i32 y1) {
    RegionScans s;
    x0 = std::max(x0, kRegionInfiniteMin);
    y0 = std::max(y0, kRegionInfiniteMin);
    x1 = std::min(x1, kRegionInfiniteMax);
    y1 = std::min(y1, kRegionInfiniteMax);
    if (x0 < x1 && y0 < y1) s.bands.push_back(ScanBand{y0, y1, {x0, x1}});
    return s;
}

RegionScans RegionScans::infinite() {
    return rect(kRegionInfiniteMin, kRegionInfiniteMin, kRegionInfiniteMax, kRegionInfiniteMax);
}

bool RegionScans::contains(i32 x, i32 y) const {
    for (const ScanBand& b : bands) {
        if (y < b.y0) return false;
        if (y >= b.y1) continue;
        for (usize i = 0; i + 1 < b.xs.size(); i += 2) {
            if (x < b.xs[i]) return false;
            if (x < b.xs[i + 1]) return true;
        }
        return false;
    }
    return false;
}

bool RegionScans::bounds(i32& x0, i32& y0, i32& x1, i32& y1) const {
    if (bands.empty()) { x0 = y0 = x1 = y1 = 0; return false; }
    y0 = bands.front().y0;
    y1 = bands.back().y1;
    x0 = bands.front().xs.front();
    x1 = bands.front().xs.back();
    for (const ScanBand& b : bands) {
        x0 = std::min(x0, b.xs.front());
        x1 = std::max(x1, b.xs.back());
    }
    return true;
}

usize RegionScans::rect_count() const {
    usize n = 0;
    for (const ScanBand& b : bands) n += b.xs.size() / 2;
    return n;
}

bool RegionScans::is_infinite() const { return *this == infinite(); }

RegionScans scans_combine(const RegionScans& a, const RegionScans& b, i32 mode) {
    if (mode == CombineModeReplace) return b;
    std::vector<i32> ys;
    ys.reserve((a.bands.size() + b.bands.size()) * 2);
    for (const ScanBand& s : a.bands) { ys.push_back(s.y0); ys.push_back(s.y1); }
    for (const ScanBand& s : b.bands) { ys.push_back(s.y0); ys.push_back(s.y1); }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    static const std::vector<i32> kNone;
    RegionScans out;
    usize ia = 0, ib = 0;
    for (usize k = 0; k + 1 < ys.size(); ++k) {
        const i32 y0 = ys[k], y1 = ys[k + 1];
        while (ia < a.bands.size() && a.bands[ia].y1 <= y0) ++ia;
        while (ib < b.bands.size() && b.bands[ib].y1 <= y0) ++ib;
        const std::vector<i32>& xa = (ia < a.bands.size() && a.bands[ia].y0 <= y0) ? a.bands[ia].xs : kNone;
        const std::vector<i32>& xb = (ib < b.bands.size() && b.bands[ib].y0 <= y0) ? b.bands[ib].xs : kNone;
        append_band(out.bands, y0, y1, span_op(xa, xb, mode));
    }
    return out;
}

// ---- Rasterizer ------------------------------------------------------------

namespace {

struct PD { double x, y; };

/// The rasterizer's 28.4 fixed-point grid (GpRound(v * 16) / 16).
double snap(double v) {
    if (std::isnan(v)) return 0;
    v = std::clamp(v, -1e9, 1e9);
    return std::floor(v * 16.0 + 0.5) / 16.0;
}

/// First pixel at or after @p v (pixel centres on integers).
i32 pix_ceil(double v) {
    const double c = std::ceil(v);
    if (c <= kRegionInfiniteMin) return kRegionInfiniteMin;
    if (c >= kRegionInfiniteMax) return kRegionInfiniteMax;
    return static_cast<i32>(c);
}

PD xform(const Affine& m, double x, double y) {
    return {m.m11 * x + m.m21 * y + m.dx, m.m12 * x + m.m22 * y + m.dy};
}

/// Bézier flattening tolerance in device pixels: GDI+'s FlatnessDefault.
constexpr double kFlatness = 0.25;

void flatten_bezier(std::vector<PD>& out, PD p0, PD p1, PD p2, PD p3, int depth) {
    const double ux = p3.x - p0.x, uy = p3.y - p0.y;
    const double len = std::hypot(ux, uy);
    double d1, d2;
    if (len < 1e-9) {
        d1 = std::hypot(p1.x - p0.x, p1.y - p0.y);
        d2 = std::hypot(p2.x - p0.x, p2.y - p0.y);
    } else {
        d1 = std::fabs((p1.x - p0.x) * uy - (p1.y - p0.y) * ux) / len;
        d2 = std::fabs((p2.x - p0.x) * uy - (p2.y - p0.y) * ux) / len;
    }
    if (depth >= 16 || std::max(d1, d2) <= kFlatness) {
        out.push_back(p3);
        return;
    }
    const PD a{(p0.x + p1.x) / 2, (p0.y + p1.y) / 2}, b{(p1.x + p2.x) / 2, (p1.y + p2.y) / 2},
        c{(p2.x + p3.x) / 2, (p2.y + p3.y) / 2};
    const PD d{(a.x + b.x) / 2, (a.y + b.y) / 2}, e{(b.x + c.x) / 2, (b.y + c.y) / 2};
    const PD f{(d.x + e.x) / 2, (d.y + e.y) / 2};
    flatten_bezier(out, p0, a, d, f, depth + 1);
    flatten_bezier(out, f, e, c, p3, depth + 1);
}

/// Device-space polygons of a path: every figure closed (a fill closes
/// open figures), curves flattened.
std::vector<std::vector<PD>> path_polygons(const GpPath& path, const Affine& m) {
    std::vector<std::vector<PD>> figures;
    std::vector<PD> cur;
    const usize n = std::min(path.points.size(), path.types.size());
    auto flush = [&] {
        if (cur.size() >= 2) figures.push_back(std::move(cur));
        cur.clear();
    };
    for (usize i = 0; i < n; ++i) {
        const u8 t = path.types[i];
        const PD p = xform(m, path.points[i].X, path.points[i].Y);
        const u8 kind = t & PathPointTypePathTypeMask;
        if (kind == PathPointTypeStart || cur.empty()) {
            flush();
            cur.push_back(p);
        } else if (kind == PathPointTypeBezier && i + 2 < n) {
            const PD p2 = xform(m, path.points[i + 1].X, path.points[i + 1].Y);
            const PD p3 = xform(m, path.points[i + 2].X, path.points[i + 2].Y);
            flatten_bezier(cur, cur.back(), p, p2, p3, 0);
            i += 2;
        } else {
            cur.push_back(p);
        }
        if (path.types[i] & PathPointTypeCloseSubpath) flush();
    }
    flush();
    return figures;
}

struct Edge {
    double x_top, y_top, dxdy;
    i32 row0, row1;  ///< rows [row0, row1) the edge crosses
    int dir;
};

RegionScans polygon_raster(const std::vector<std::vector<PD>>& figures, bool winding) {
    std::vector<Edge> edges;
    for (const auto& fig : figures) {
        for (usize i = 0; i < fig.size(); ++i) {
            PD a{snap(fig[i].x), snap(fig[i].y)};
            const PD& q = fig[(i + 1) % fig.size()];
            PD b{snap(q.x), snap(q.y)};
            if (a.y == b.y) continue;
            int dir = 1;
            if (a.y > b.y) { std::swap(a, b); dir = -1; }
            Edge e{a.x, a.y, (b.x - a.x) / (b.y - a.y), pix_ceil(a.y), pix_ceil(b.y), dir};
            if (e.row0 < e.row1) edges.push_back(e);
        }
    }
    RegionScans out;
    if (edges.empty()) return out;
    std::sort(edges.begin(), edges.end(), [](const Edge& l, const Edge& r) { return l.row0 < r.row0; });
    i32 row_end = edges.front().row1;
    for (const Edge& e : edges) row_end = std::max(row_end, e.row1);
    std::vector<const Edge*> active;
    std::vector<std::pair<double, int>> xs;
    usize next = 0;
    for (i32 y = edges.front().row0; y < row_end; ++y) {
        if (active.empty() && next < edges.size() && edges[next].row0 > y) y = edges[next].row0;
        while (next < edges.size() && edges[next].row0 <= y) active.push_back(&edges[next++]);
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [y](const Edge* e) { return e->row1 <= y; }),
                     active.end());
        if (active.empty()) continue;
        xs.clear();
        for (const Edge* e : active) xs.emplace_back(e->x_top + (y - e->y_top) * e->dxdy, e->dir);
        std::sort(xs.begin(), xs.end());
        std::vector<i32> spans;
        auto add = [&spans](double l, double r) {
            const i32 x0 = pix_ceil(l), x1 = pix_ceil(r);
            if (x0 >= x1) return;
            if (!spans.empty() && spans.back() >= x0) {
                spans.back() = std::max(spans.back(), x1);
                return;
            }
            spans.push_back(x0);
            spans.push_back(x1);
        };
        if (winding) {
            int w = 0;
            double start = 0;
            for (const auto& [x, d] : xs) {
                const int before = w;
                w += d;
                if (before == 0 && w != 0) start = x;
                else if (before != 0 && w == 0) add(start, x);
            }
        } else {
            for (usize i = 0; i + 1 < xs.size(); i += 2) add(xs[i].first, xs[i + 1].first);
        }
        append_band(out.bands, y, y + 1, std::move(spans));
    }
    return out;
}

}  // namespace

RegionScans rect_raster(const RectF& r, const Affine& m) {
    if (!(r.Width > 0) || !(r.Height > 0)) return {};
    if (m.m12 == 0 && m.m21 == 0) {
        const PD a = xform(m, r.X, r.Y), b = xform(m, double(r.X) + r.Width, double(r.Y) + r.Height);
        return RegionScans::rect(pix_ceil(snap(std::min(a.x, b.x))), pix_ceil(snap(std::min(a.y, b.y))),
                                 pix_ceil(snap(std::max(a.x, b.x))), pix_ceil(snap(std::max(a.y, b.y))));
    }
    const double x1 = double(r.X) + r.Width, y1 = double(r.Y) + r.Height;
    std::vector<std::vector<PD>> fig{
        {xform(m, r.X, r.Y), xform(m, x1, r.Y), xform(m, x1, y1), xform(m, r.X, y1)}};
    return polygon_raster(fig, false);
}

RegionScans path_raster(const GpPath& path, const Affine& m) {
    return polygon_raster(path_polygons(path, m), path.fill_mode == FillModeWinding);
}

RegionScans region_raster(const GpRegion& r, const Affine& m) {
    switch (r.kind) {
    case GpRegion::Kind::Rect: return rect_raster(r.rect, m);
    case GpRegion::Kind::Path: return path_raster(r.path, m);
    case GpRegion::Kind::Empty: return {};
    case GpRegion::Kind::Infinite: return RegionScans::infinite();
    case GpRegion::Kind::Combine:
        if (!r.left || !r.right) return {};
        return scans_combine(region_raster(*r.left, m), region_raster(*r.right, m), r.mode);
    }
    return {};
}

const RegionScans& region_device_scans(const GpRegion& r) {
    if (!r.identity_scans) r.identity_scans = std::make_shared<const RegionScans>(region_raster(r, Affine{}));
    return *r.identity_scans;
}

// ---- Tree operations -------------------------------------------------------

std::shared_ptr<GpRegion> region_clone(const GpRegion& r) {
    auto c = std::make_shared<GpRegion>();
    c->kind = r.kind;
    c->mode = r.mode;
    c->rect = r.rect;
    c->path = r.path;
    if (r.left) c->left = region_clone(*r.left);
    if (r.right) c->right = region_clone(*r.right);
    c->identity_scans = r.identity_scans;  // immutable, same pixels
    return c;
}

void region_transform(GpRegion& r, const Affine& m) {
    r.identity_scans.reset();
    switch (r.kind) {
    case GpRegion::Kind::Rect:
        if (m.m12 == 0 && m.m21 == 0 && m.m11 != 0 && m.m22 != 0) {
            const PD a = xform(m, r.rect.X, r.rect.Y);
            const PD b = xform(m, double(r.rect.X) + r.rect.Width, double(r.rect.Y) + r.rect.Height);
            r.rect = {static_cast<float>(std::min(a.x, b.x)), static_cast<float>(std::min(a.y, b.y)),
                      static_cast<float>(std::fabs(b.x - a.x)), static_cast<float>(std::fabs(b.y - a.y))};
        } else {
            // A rect under rotation/shear becomes its four corners: Start,
            // Line, Line, Line, no CloseSubpath (probe "rot rect").
            const RectF q = r.rect;
            const PointF c[4] = {{q.X, q.Y}, {q.X + q.Width, q.Y}, {q.X + q.Width, q.Y + q.Height},
                                 {q.X, q.Y + q.Height}};
            r.kind = GpRegion::Kind::Path;
            r.path = GpPath{};
            for (const PointF& p : c) r.path.points.push_back(m.apply(p));
            r.path.types = {PathPointTypeStart, PathPointTypeLine, PathPointTypeLine, PathPointTypeLine};
            r.path.new_figure = false;
        }
        break;
    case GpRegion::Kind::Path:
        for (PointF& p : r.path.points) p = m.apply(p);
        break;
    case GpRegion::Kind::Combine:
        if (r.left) region_transform(*r.left, m);
        if (r.right) region_transform(*r.right, m);
        break;
    default: break;  // Empty, Infinite
    }
}

void region_translate(GpRegion& r, float dx, float dy) {
    r.identity_scans.reset();
    switch (r.kind) {
    case GpRegion::Kind::Rect:
        r.rect.X += dx;
        r.rect.Y += dy;
        break;
    case GpRegion::Kind::Path:
        for (PointF& p : r.path.points) { p.X += dx; p.Y += dy; }
        break;
    case GpRegion::Kind::Combine:
        if (r.left) region_translate(*r.left, dx, dy);
        if (r.right) region_translate(*r.right, dx, dy);
        break;
    default: break;
    }
}

void region_combine(GpRegion& a, const GpRegion& b, i32 mode) {
    using K = GpRegion::Kind;
    auto copy_b = [&] { a = std::move(*region_clone(b)); };
    auto set_empty = [&] { a = GpRegion{}; a.kind = K::Empty; };
    auto set_infinite = [&] { a = GpRegion{}; };
    const bool ae = a.kind == K::Empty, ai = a.kind == K::Infinite;
    const bool be = b.kind == K::Empty, bi = b.kind == K::Infinite;
    // Folding rules: gdiplus_region_probe "inf.*", "empty.*", "rect.*.inf",
    // "rect.*.empty".
    switch (mode) {
    case CombineModeReplace: copy_b(); return;
    case CombineModeIntersect:
        if (ae || be) { set_empty(); return; }
        if (ai) { copy_b(); return; }
        if (bi) return;
        break;
    case CombineModeUnion:
        if (ai || bi) { set_infinite(); return; }
        if (ae) { copy_b(); return; }
        if (be) return;
        break;
    case CombineModeXor:
        if (ae) { copy_b(); return; }
        if (be) return;
        break;
    case CombineModeExclude:
        if (ae || bi) { set_empty(); return; }
        if (be) return;
        break;
    case CombineModeComplement:
        if (be || ai) { set_empty(); return; }
        if (ae) { copy_b(); return; }
        break;
    default: return;
    }
    auto left = std::make_shared<GpRegion>(std::move(a));
    a = GpRegion{};
    a.kind = K::Combine;
    a.mode = mode;
    a.left = std::move(left);
    a.right = region_clone(b);
    a.identity_scans.reset();
}

GpRegion region_from_rects(const std::vector<std::array<i32, 4>>& ltrb) {
    GpRegion r;
    r.kind = GpRegion::Kind::Path;
    r.path.fill_mode = FillModeWinding;
    for (const auto& q : ltrb) {
        const float l = static_cast<float>(q[0]), t = static_cast<float>(q[1]);
        const float rr = static_cast<float>(q[2]), b = static_cast<float>(q[3]);
        r.path.points.insert(r.path.points.end(), {{l, t}, {rr, t}, {rr, b}, {l, b}});
        r.path.types.insert(r.path.types.end(),
                            {PathPointTypeStart, PathPointTypeLine, PathPointTypeLine,
                             static_cast<u8>(PathPointTypeLine | PathPointTypeCloseSubpath)});
    }
    r.path.new_figure = true;
    return r;
}

// ---- Serialization (GetRegionData / CreateRegionRgnData) --------------------
//
// Layout (probe "rect", "pathrect", "rr.*", "tree*"):
//   u32 size (bytes after this and the checksum), u32 checksum (CRC-32,
//   reflected poly 0xEDB88320, initial 0, no final xor, over everything
//   after it), u32 version 0xDBC01002, u32 2 * combine-node count, then the
//   nodes in preorder: combine = u32 mode + two subtrees; rect =
//   0x10000000 + 4 REALs; path = 0x10000001 + u32 path bytes + path;
//   empty = 0x10000002; infinite = 0x10000003.
// Path: u32 version, u32 count, u32 flags (0x4000 INT16 points when every
// point is integral and fits, 0x2000 winding), points, type bytes padded
// with zeros to 4.

namespace {

constexpr u32 kRegionVersion = 0xDBC01002;
constexpr u32 kPathFlagCompressed = 0x4000;
constexpr u32 kPathFlagWinding = 0x2000;
constexpr u32 kPathFlagRelative = 0x0800;

bool version_ok(u32 v) { return (v & 0xFFFFF000u) == 0xDBC01000u; }

u32 crc32_gdip(const u8* p, usize n) {
    u32 c = 0;
    for (usize i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
    }
    return c;
}

void put32(std::vector<u8>& o, u32 v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<u8>(v >> (8 * i)));
}
void putf(std::vector<u8>& o, float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    put32(o, b);
}

bool fits_i16(float v) { return v >= -32768.0f && v <= 32767.0f && std::nearbyint(v) == v; }

void put_path(std::vector<u8>& o, const GpPath& p) {
    const usize n = std::min(p.points.size(), p.types.size());
    bool compressed = n > 0;
    for (usize i = 0; i < n && compressed; ++i)
        compressed = fits_i16(p.points[i].X) && fits_i16(p.points[i].Y);
    put32(o, kRegionVersion);
    put32(o, static_cast<u32>(n));
    put32(o, (compressed ? kPathFlagCompressed : 0) | (p.fill_mode == FillModeWinding ? kPathFlagWinding : 0));
    for (usize i = 0; i < n; ++i) {
        if (compressed) {
            const u16 x = static_cast<u16>(static_cast<i16>(p.points[i].X));
            const u16 y = static_cast<u16>(static_cast<i16>(p.points[i].Y));
            put32(o, static_cast<u32>(x) | (static_cast<u32>(y) << 16));
        } else {
            putf(o, p.points[i].X);
            putf(o, p.points[i].Y);
        }
    }
    for (usize i = 0; i < n; ++i) o.push_back(p.types[i]);
    while (o.size() % 4 != 0) o.push_back(0);
}

void put_node(std::vector<u8>& o, const GpRegion& r) {
    switch (r.kind) {
    case GpRegion::Kind::Combine:
        put32(o, static_cast<u32>(r.mode));
        put_node(o, *r.left);
        put_node(o, *r.right);
        break;
    case GpRegion::Kind::Rect:
        put32(o, static_cast<u32>(r.kind));
        putf(o, r.rect.X);
        putf(o, r.rect.Y);
        putf(o, r.rect.Width);
        putf(o, r.rect.Height);
        break;
    case GpRegion::Kind::Path: {
        put32(o, static_cast<u32>(r.kind));
        const usize at = o.size();
        put32(o, 0);
        put_path(o, r.path);
        const u32 n = static_cast<u32>(o.size() - at - 4);
        for (int i = 0; i < 4; ++i) o[at + i] = static_cast<u8>(n >> (8 * i));
        break;
    }
    default: put32(o, static_cast<u32>(r.kind)); break;
    }
}

u32 combine_count(const GpRegion& r) {
    if (r.kind != GpRegion::Kind::Combine) return 0;
    return 1 + combine_count(*r.left) + combine_count(*r.right);
}

struct Reader {
    const u8* p;
    usize n;
    usize at{0};
    bool get32(u32& v) {
        if (n - at < 4) return false;
        v = static_cast<u32>(p[at]) | (static_cast<u32>(p[at + 1]) << 8) |
            (static_cast<u32>(p[at + 2]) << 16) | (static_cast<u32>(p[at + 3]) << 24);
        at += 4;
        return true;
    }
    bool getf(float& f) {
        u32 v;
        if (!get32(v)) return false;
        std::memcpy(&f, &v, 4);
        return true;
    }
};

bool get_path(Reader& rd, GpPath& path) {
    u32 size = 0, version = 0, count = 0, flags = 0;
    if (!rd.get32(size) || size > rd.n - rd.at) return false;
    Reader pr{rd.p + rd.at, size};
    rd.at += size;
    if (!pr.get32(version) || !pr.get32(count) || !pr.get32(flags) || !version_ok(version)) return false;
    if (flags & kPathFlagRelative) {
        // EMF+ relative-coordinate points: GDI+ never writes them into
        // region data (GetRegionData uses only the INT16/REAL forms).
        log_unimplemented("GdipCreateRegionRgnData", "relative (0x0800) path point encoding");
        return false;
    }
    const bool compressed = (flags & kPathFlagCompressed) != 0;
    const u64 need = 12ull + u64(count) * (compressed ? 4u : 8u) + ((u64(count) + 3) & ~3ull);
    if (need != size) return false;
    path = GpPath{};
    path.fill_mode = (flags & kPathFlagWinding) ? FillModeWinding : FillModeAlternate;
    path.points.resize(count);
    for (u32 i = 0; i < count; ++i) {
        if (compressed) {
            u32 v;
            pr.get32(v);
            path.points[i] = {static_cast<float>(static_cast<i16>(v & 0xFFFF)),
                              static_cast<float>(static_cast<i16>(v >> 16))};
        } else {
            pr.getf(path.points[i].X);
            pr.getf(path.points[i].Y);
        }
    }
    path.types.assign(pr.p + pr.at, pr.p + pr.at + count);
    for (u8 t : path.types) {
        const u8 kind = t & PathPointTypePathTypeMask;
        if (kind != PathPointTypeStart && kind != PathPointTypeLine && kind != PathPointTypeBezier)
            return false;
    }
    path.new_figure = true;
    return true;
}

bool get_node(Reader& rd, GpRegion& r, u32& combines, int depth) {
    u32 type = 0;
    if (depth > 4096 || !rd.get32(type)) return false;
    switch (type) {
    case CombineModeIntersect: case CombineModeUnion: case CombineModeXor:
    case CombineModeExclude: case CombineModeComplement:
        r.kind = GpRegion::Kind::Combine;
        r.mode = static_cast<i32>(type);
        r.left = std::make_shared<GpRegion>();
        r.right = std::make_shared<GpRegion>();
        ++combines;
        return get_node(rd, *r.left, combines, depth + 1) && get_node(rd, *r.right, combines, depth + 1);
    case static_cast<u32>(GpRegion::Kind::Rect):
        r.kind = GpRegion::Kind::Rect;
        return rd.getf(r.rect.X) && rd.getf(r.rect.Y) && rd.getf(r.rect.Width) && rd.getf(r.rect.Height);
    case static_cast<u32>(GpRegion::Kind::Path):
        r.kind = GpRegion::Kind::Path;
        return get_path(rd, r.path);
    case static_cast<u32>(GpRegion::Kind::Empty): r.kind = GpRegion::Kind::Empty; return true;
    case static_cast<u32>(GpRegion::Kind::Infinite): r.kind = GpRegion::Kind::Infinite; return true;
    default:
        // GDI+ stores an unknown node type unparsed and fails its getters
        // later (probe "rgndata node bad"); refusing it here keeps every
        // region this side well-formed.
        return false;
    }
}

}  // namespace

std::vector<u8> region_serialize(const GpRegion& r) {
    std::vector<u8> o;
    put32(o, 0);
    put32(o, 0);
    put32(o, kRegionVersion);
    put32(o, 2 * combine_count(r));
    put_node(o, r);
    const u32 size = static_cast<u32>(o.size() - 8);
    const u32 crc = crc32_gdip(o.data() + 8, o.size() - 8);
    for (int i = 0; i < 4; ++i) {
        o[i] = static_cast<u8>(size >> (8 * i));
        o[4 + i] = static_cast<u8>(crc >> (8 * i));
    }
    return o;
}

bool region_deserialize(const u8* data, usize size, GpRegion& out) {
    if (data == nullptr || size < 16) return false;
    Reader hdr{data, size};
    u32 body = 0, crc = 0, version = 0, count = 0;
    hdr.get32(body);
    hdr.get32(crc);
    if (body < 8 || u64(body) + 8 > size) return false;
    if (crc32_gdip(data + 8, body) != crc) return false;
    Reader rd{data, body + 8, 8};
    rd.get32(version);
    rd.get32(count);
    if (!version_ok(version)) return false;
    GpRegion r;
    u32 combines = 0;
    if (!get_node(rd, r, combines, 0)) return false;
    if (count != 2 * combines) return false;
    out = std::move(r);
    return true;
}

// ---- CoreGraphics ----------------------------------------------------------

void clip_to_region(CGContextRef cg, const GpRegion* region) {
    if (cg == nullptr || region == nullptr || region->kind == GpRegion::Kind::Infinite) return;
    const RegionScans& s = region_device_scans(*region);
    if (s.empty()) {
        CGContextClipToRect(cg, CGRectZero);
        return;
    }
    std::vector<CGRect> rects;
    rects.reserve(s.rect_count());
    for (const ScanBand& b : s.bands)
        for (usize i = 0; i + 1 < b.xs.size(); i += 2)
            rects.push_back(CGRectMake(b.xs[i], b.y0, b.xs[i + 1] - b.xs[i], b.y1 - b.y0));
    CGContextClipToRects(cg, rects.data(), rects.size());
}

CGMutablePathRef region_to_cg(const GpRegion& region) {
    if (region.kind == GpRegion::Kind::Infinite) return nullptr;
    CGMutablePathRef path = CGPathCreateMutable();
    const RegionScans& s = region_device_scans(region);
    for (const ScanBand& b : s.bands)
        for (usize i = 0; i + 1 < b.xs.size(); i += 2)
            CGPathAddRect(path, nullptr, CGRectMake(b.xs[i], b.y0, b.xs[i + 1] - b.xs[i], b.y1 - b.y0));
    return path;
}

}  // namespace vitreo::winapi::gdip
