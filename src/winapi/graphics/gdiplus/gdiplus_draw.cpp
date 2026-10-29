// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_draw.cpp
/// @brief GDI+ area: Draw* / Fill* primitives and GdipGraphicsClear.
///
/// Each primitive builds its figure with the geometry area's path builders
/// (the same points GdipAddPath* emits), strokes it through stroke_path or
/// fills it through the brush area's fill_path. Argument rules are the
/// Microsoft Learn Graphics:: method pages plus the winoracle probe
/// tools/oracle_probe/gdiplus_draw_probe.c (.expected): minimum counts,
/// Bézier 3n+1 counts, Curve3 offset/segment range, Arc/Pie positive size,
/// fill-mode range, and ObjectBusy for an object of the wrong class.

#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_geometry.hpp"
#include "winapi/graphics/gdiplus/gdiplus_graphics.hpp"
#include "winapi/graphics/gdiplus/gdiplus_pen.hpp"
#include "winapi/graphics/gdiplus/gdiplus_pen_brush.hpp"
#include "winapi/graphics/gdiplus/gdiplus_render.hpp"
#include "winapi/core/marshaler.hpp"

#include <CoreGraphics/CoreGraphics.h>

namespace vitreo::winapi::gdip {

namespace {

/// Default cardinal-spline tension of DrawCurve / DrawClosedCurve /
/// FillClosedCurve (Learn Graphics::DrawCurve: 0.5).
constexpr float kDefaultTension = 0.5f;

void reg(HandlerMap& map, const char* name, std::function<Status(Marshaler&)> fn) {
    map[name] = [fn = std::move(fn)](Marshaler& m) { write_status(m, fn(m)); };
}

template <typename T>
Status object_arg(GdiplusManager& mgr, Marshaler& m, usize i, T*& out) {
    const u64 h = handle_arg(m, i);
    out = mgr.objects().get<T>(h);
    return resolve_status(mgr, h, out != nullptr);
}

/// Graphics at 0 and pen at 1, the head of every Draw* export.
Status gp_pen(GdiplusManager& mgr, Marshaler& m, GpGraphics*& g, GpPen*& pen) {
    if (Status s = object_arg(mgr, m, 0, g); s != Status::Ok) return s;
    return object_arg(mgr, m, 1, pen);
}
Status gp_brush(GdiplusManager& mgr, Marshaler& m, GpGraphics*& g, GpBrush*& brush) {
    if (Status s = object_arg(mgr, m, 0, g); s != Status::Ok) return s;
    return object_arg(mgr, m, 1, brush);
}

Status stroke(GdiplusManager& mgr, Marshaler& m, GpGraphics& g, const GpPath& path,
              const GpPen& pen) {
    RenderScope scope(mgr, m, g);
    if (scope.status() != Status::Ok) return scope.status();
    const Status s = stroke_path(scope, path, pen);
    const Status c = scope.commit();
    return s != Status::Ok ? s : c;
}

Status fill(GdiplusManager& mgr, Marshaler& m, GpGraphics& g, const GpPath& path,
            bool even_odd, const GpBrush& brush) {
    RenderScope scope(mgr, m, g);
    if (scope.status() != Status::Ok) return scope.status();
    Status s = Status::Ok;
    if (scope.cg() != nullptr && !path.points.empty()) {
        CGMutablePathRef cg = path_to_cg(path);
        if (cg == nullptr) return Status::OutOfMemory;
        s = paint_path(scope, cg, even_odd, brush);
        CGPathRelease(cg);
    }
    const Status c = scope.commit();
    return s != Status::Ok ? s : c;
}

/// Points at @p at_index / count at @p count_index (REAL or INT pairs).
Status points_arg(Marshaler& m, usize at_index, usize count_index, bool ints, i32 min_count,
                  std::vector<PointF>& pts) {
    const GuestAddr at = m.arg_ptr(at_index);
    const i32 count = static_cast<i32>(m.arg32(count_index));
    if (at == 0 || count < min_count || count <= 0) return Status::InvalidParameter;
    const bool ok = ints ? read_points_i(m, at, count, pts) : read_points(m, at, count, pts);
    return ok ? Status::Ok : Status::InvalidParameter;
}

/// x, y, width, height at @p first (REAL or INT).
RectF rect_args(Marshaler& m, usize first, bool ints) {
    if (ints) {
        return {static_cast<float>(static_cast<i32>(m.arg32(first))),
                static_cast<float>(static_cast<i32>(m.arg32(first + 1))),
                static_cast<float>(static_cast<i32>(m.arg32(first + 2))),
                static_cast<float>(static_cast<i32>(m.arg32(first + 3)))};
    }
    return {real_arg(m, first), real_arg(m, first + 1), real_arg(m, first + 2),
            real_arg(m, first + 3)};
}

Status rects_arg(Marshaler& m, usize at_index, usize count_index, bool ints,
                 std::vector<RectF>& rects) {
    const GuestAddr at = m.arg_ptr(at_index);
    const i32 count = static_cast<i32>(m.arg32(count_index));
    if (at == 0 || count <= 0) return Status::InvalidParameter;
    const bool ok = ints ? read_rects_i(m, at, count, rects) : read_rects(m, at, count, rects);
    return ok ? Status::Ok : Status::InvalidParameter;
}

bool valid_fill_mode(i32 mode) { return mode == FillModeAlternate || mode == FillModeWinding; }

const char* name2(const char* base, bool ints, std::string& keep) {
    keep = std::string(base) + (ints ? "I" : "");
    return keep.c_str();
}

}  // namespace

void register_draw(GdiplusManager& mgr, HandlerMap& map) {
    register_pen(mgr, map);
    GdiplusManager* pm = &mgr;
    std::string n;

    for (bool ints : {false, true}) {
        // GdipDrawLine(g, pen, x1, y1, x2, y2)
        reg(map, name2("GdipDrawLine", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            const RectF r = rect_args(m, 2, ints);
            GpPath path;
            path_add_lines(path, {{r.X, r.Y}, {r.Width, r.Height}});
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawLines(g, pen, points, count): count >= 2.
        reg(map, name2("GdipDrawLines", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 2, pts); s != Status::Ok) return s;
            GpPath path;
            path_add_lines(path, pts);
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawArc(g, pen, x, y, w, h, start, sweep): w, h > 0.
        reg(map, name2("GdipDrawArc", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            const RectF r = rect_args(m, 2, ints);
            if (!(r.Width > 0) || !(r.Height > 0)) return Status::InvalidParameter;
            GpPath path;
            path_add_arc(path, r, real_arg(m, 6), real_arg(m, 7));
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawPie(g, pen, x, y, w, h, start, sweep): w, h > 0.
        reg(map, name2("GdipDrawPie", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            const RectF r = rect_args(m, 2, ints);
            if (!(r.Width > 0) || !(r.Height > 0)) return Status::InvalidParameter;
            GpPath path;
            path_add_pie(path, r, real_arg(m, 6), real_arg(m, 7));
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawBezier(g, pen, x1, y1, ..., x4, y4)
        reg(map, name2("GdipDrawBezier", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            const RectF a = rect_args(m, 2, ints), b = rect_args(m, 6, ints);
            GpPath path;
            path_add_beziers(path, {{a.X, a.Y}, {a.Width, a.Height}, {b.X, b.Y}, {b.Width, b.Height}});
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawBeziers(g, pen, points, count): count > 0; above 3 it
        // must be 3n+1; 1..3 points draw nothing (probe drawbeziers n=*).
        reg(map, name2("GdipDrawBeziers", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 1, pts); s != Status::Ok) return s;
            if (pts.size() > 3 && (pts.size() - 1) % 3 != 0) return Status::InvalidParameter;
            if (pts.size() < 4) return Status::Ok;
            GpPath path;
            path_add_beziers(path, pts);
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawRectangle(g, pen, x, y, w, h): any size (probe w=-1/0 Ok).
        reg(map, name2("GdipDrawRectangle", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            GpPath path;
            path_add_rectangle(path, rect_args(m, 2, ints));
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawRectangles(g, pen, rects, count): count > 0.
        reg(map, name2("GdipDrawRectangles", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            std::vector<RectF> rects;
            if (Status s = rects_arg(m, 2, 3, ints, rects); s != Status::Ok) return s;
            GpPath path;
            for (const RectF& r : rects) path_add_rectangle(path, r);
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawEllipse(g, pen, x, y, w, h): any size (probe w=0/-2 Ok).
        reg(map, name2("GdipDrawEllipse", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            GpPath path;
            path_add_ellipse(path, rect_args(m, 2, ints));
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawPolygon(g, pen, points, count): count >= 2.
        reg(map, name2("GdipDrawPolygon", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 2, pts); s != Status::Ok) return s;
            GpPath path;
            path_add_polygon(path, pts);
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawCurve(g, pen, points, count) / Curve2(..., tension): count >= 2.
        reg(map, name2("GdipDrawCurve", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 2, pts); s != Status::Ok) return s;
            GpPath path;
            path_add_curve(path, pts, 0, static_cast<i32>(pts.size()) - 1, kDefaultTension);
            return stroke(*pm, m, *g, path, *pen);
        });
        reg(map, name2("GdipDrawCurve2", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 2, pts); s != Status::Ok) return s;
            GpPath path;
            path_add_curve(path, pts, 0, static_cast<i32>(pts.size()) - 1, real_arg(m, 4));
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawCurve3(g, pen, points, count, offset, segments, tension):
        // offset >= 0, segments >= 1, offset + segments <= count - 1.
        reg(map, name2("GdipDrawCurve3", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 2, pts); s != Status::Ok) return s;
            const i32 offset = static_cast<i32>(m.arg32(4));
            const i32 segments = static_cast<i32>(m.arg32(5));
            const i32 count = static_cast<i32>(pts.size());
            if (offset < 0 || segments < 1 || offset + segments > count - 1) {
                return Status::InvalidParameter;
            }
            GpPath path;
            path_add_curve(path, pts, offset, segments, real_arg(m, 6));
            return stroke(*pm, m, *g, path, *pen);
        });
        // GdipDrawClosedCurve(g, pen, points, count) / 2(..., tension): count >= 3.
        reg(map, name2("GdipDrawClosedCurve", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 3, pts); s != Status::Ok) return s;
            GpPath path;
            path_add_closed_curve(path, pts, kDefaultTension);
            return stroke(*pm, m, *g, path, *pen);
        });
        reg(map, name2("GdipDrawClosedCurve2", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpPen* pen = nullptr;
            if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 3, pts); s != Status::Ok) return s;
            GpPath path;
            path_add_closed_curve(path, pts, real_arg(m, 4));
            return stroke(*pm, m, *g, path, *pen);
        });

        // ---- Fill ------------------------------------------------------------
        // GdipFillRectangle(g, brush, x, y, w, h): any size.
        reg(map, name2("GdipFillRectangle", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpBrush* b = nullptr;
            if (Status s = gp_brush(*pm, m, g, b); s != Status::Ok) return s;
            GpPath path;
            path_add_rectangle(path, rect_args(m, 2, ints));
            return fill(*pm, m, *g, path, false, *b);
        });
        // GdipFillRectangles(g, brush, rects, count): count > 0. The union
        // of the rectangles is painted once.
        reg(map, name2("GdipFillRectangles", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpBrush* b = nullptr;
            if (Status s = gp_brush(*pm, m, g, b); s != Status::Ok) return s;
            std::vector<RectF> rects;
            if (Status s = rects_arg(m, 2, 3, ints, rects); s != Status::Ok) return s;
            GpPath path;
            for (RectF r : rects) {
                if (r.Width < 0) { r.X += r.Width; r.Width = -r.Width; }
                if (r.Height < 0) { r.Y += r.Height; r.Height = -r.Height; }
                path_add_rectangle(path, r);
            }
            return fill(*pm, m, *g, path, false, *b);
        });
        // GdipFillPolygon(g, brush, points, count, fillMode): count > 0,
        // fill mode Alternate/Winding; fewer than 3 points paint nothing.
        reg(map, name2("GdipFillPolygon", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpBrush* b = nullptr;
            if (Status s = gp_brush(*pm, m, g, b); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 1, pts); s != Status::Ok) return s;
            const i32 mode = static_cast<i32>(m.arg32(4));
            if (!valid_fill_mode(mode)) return Status::InvalidParameter;
            if (pts.size() < 3) return Status::Ok;
            GpPath path;
            path_add_polygon(path, pts);
            return fill(*pm, m, *g, path, mode == FillModeAlternate, *b);
        });
        // GdipFillPolygon2(g, brush, points, count): Alternate.
        reg(map, name2("GdipFillPolygon2", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpBrush* b = nullptr;
            if (Status s = gp_brush(*pm, m, g, b); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 1, pts); s != Status::Ok) return s;
            if (pts.size() < 3) return Status::Ok;
            GpPath path;
            path_add_polygon(path, pts);
            return fill(*pm, m, *g, path, true, *b);
        });
        // GdipFillEllipse(g, brush, x, y, w, h): any size.
        reg(map, name2("GdipFillEllipse", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpBrush* b = nullptr;
            if (Status s = gp_brush(*pm, m, g, b); s != Status::Ok) return s;
            GpPath path;
            path_add_ellipse(path, rect_args(m, 2, ints));
            return fill(*pm, m, *g, path, true, *b);
        });
        // GdipFillPie(g, brush, x, y, w, h, start, sweep): w, h > 0.
        reg(map, name2("GdipFillPie", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpBrush* b = nullptr;
            if (Status s = gp_brush(*pm, m, g, b); s != Status::Ok) return s;
            const RectF r = rect_args(m, 2, ints);
            if (!(r.Width > 0) || !(r.Height > 0)) return Status::InvalidParameter;
            GpPath path;
            path_add_pie(path, r, real_arg(m, 6), real_arg(m, 7));
            return fill(*pm, m, *g, path, true, *b);
        });
        // GdipFillClosedCurve(g, brush, points, count): count > 0, Alternate;
        // GdipFillClosedCurve2(..., tension, fillMode). Fewer than 3 points
        // paint nothing (probe fillclosedcurve n=1/2 Ok).
        reg(map, name2("GdipFillClosedCurve", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpBrush* b = nullptr;
            if (Status s = gp_brush(*pm, m, g, b); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 1, pts); s != Status::Ok) return s;
            if (pts.size() < 3) return Status::Ok;
            GpPath path;
            path_add_closed_curve(path, pts, kDefaultTension);
            return fill(*pm, m, *g, path, true, *b);
        });
        reg(map, name2("GdipFillClosedCurve2", ints, n), [pm, ints](Marshaler& m) {
            GpGraphics* g = nullptr;
            GpBrush* b = nullptr;
            if (Status s = gp_brush(*pm, m, g, b); s != Status::Ok) return s;
            std::vector<PointF> pts;
            if (Status s = points_arg(m, 2, 3, ints, 1, pts); s != Status::Ok) return s;
            const float tension = real_arg(m, 4);
            const i32 mode = static_cast<i32>(m.arg32(5));
            if (!valid_fill_mode(mode)) return Status::InvalidParameter;
            if (pts.size() < 3) return Status::Ok;
            GpPath path;
            path_add_closed_curve(path, pts, tension);
            return fill(*pm, m, *g, path, mode == FillModeAlternate, *b);
        });
    }

    // GdipDrawPath(g, pen, path)
    reg(map, "GdipDrawPath", [pm](Marshaler& m) {
        GpGraphics* g = nullptr;
        GpPen* pen = nullptr;
        if (Status s = gp_pen(*pm, m, g, pen); s != Status::Ok) return s;
        GpPath* path = nullptr;
        if (Status s = object_arg(*pm, m, 2, path); s != Status::Ok) return s;
        return stroke(*pm, m, *g, *path, *pen);
    });
    // GdipFillPath(g, brush, path): the path's own fill mode.
    reg(map, "GdipFillPath", [pm](Marshaler& m) {
        GpGraphics* g = nullptr;
        GpBrush* b = nullptr;
        if (Status s = gp_brush(*pm, m, g, b); s != Status::Ok) return s;
        GpPath* path = nullptr;
        if (Status s = object_arg(*pm, m, 2, path); s != Status::Ok) return s;
        return fill(*pm, m, *g, *path, path->fill_mode == FillModeAlternate, *b);
    });
    // GdipGraphicsClear(g, argb): every pixel inside the clip becomes
    // @p argb exactly, whatever the compositing mode (probe clear clipped:
    // 80ff0000 over opaque blue reads back 80ff0000).
    reg(map, "GdipGraphicsClear", [pm](Marshaler& m) {
        GpGraphics* g = nullptr;
        if (Status s = object_arg(*pm, m, 0, g); s != Status::Ok) return s;
        const u32 argb = m.arg32(1);
        RenderScope scope(*pm, m, *g);
        if (scope.status() != Status::Ok) return scope.status();
        CGContextRef cg = scope.cg();
        if (cg != nullptr) {
            CGContextSaveGState(cg);
            CGContextConcatCTM(cg, CGAffineTransformInvert(CGContextGetCTM(cg)));
            CGContextSetBlendMode(cg, kCGBlendModeCopy);
            CGContextSetShouldAntialias(cg, false);
            CGContextSetRGBFillColor(cg, ((argb >> 16) & 0xFF) / 255.0,
                                     ((argb >> 8) & 0xFF) / 255.0, (argb & 0xFF) / 255.0,
                                     ((argb >> 24) & 0xFF) / 255.0);
            CGContextFillRect(cg, CGRectMake(0, 0, scope.width(), scope.height()));
            CGContextRestoreGState(cg);
        }
        return scope.commit();
    });
}

}  // namespace vitreo::winapi::gdip
