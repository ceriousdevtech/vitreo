#pragma once
/// @file gdiplus_geometry.hpp
/// @brief GDI+ geometry objects: GpMatrix, GpPath (GraphicsPath) and
/// GpPathIterator, plus the path builders the Draw*/Fill* exports share.
///
/// Contract sources: Windows SDK GdiPlusFlat.h / GdiPlusPath.h /
/// GdiPlusMatrix.h / GdiPlusEnums.h, the Microsoft Learn GraphicsPath /
/// Matrix / GraphicsPathIterator pages, and the winoracle probe
/// tools/oracle_probe/gdiplus_geometry_probe.c (.expected) for the exact
/// point arrays GDI+ emits for arcs, ellipses, pies and cardinal splines.

#include "winapi/graphics/gdiplus/gdiplus_core.hpp"

#include <memory>
#include <vector>

typedef struct CGPath* CGMutablePathRef;

namespace vitreo::winapi::gdip {

/// GDI+ `PathPointType` (GdiPlusEnums.h).
enum PathPointType : u8 {
    PathPointTypeStart = 0,
    PathPointTypeLine = 1,
    PathPointTypeBezier = 3,
    PathPointTypePathTypeMask = 0x07,
    PathPointTypeDashMode = 0x10,
    PathPointTypePathMarker = 0x20,
    PathPointTypeCloseSubpath = 0x80,
};

/// GDI+ `FillMode`.
enum FillMode : i32 { FillModeAlternate = 0, FillModeWinding = 1 };

/// GDI+ default flatness (FlatnessDefault, GdiPlusEnums.h).
inline constexpr float kFlatnessDefault = 0.25f;

struct GpMatrix {
    Affine m;
};

struct GpPath {
    std::vector<PointF> points;
    /// PathPointType bytes (Start/Line/Bezier | CloseSubpath | PathMarker | DashMode).
    std::vector<u8> types;
    i32 fill_mode{FillModeAlternate};
    /// The next added point starts a new figure (after create/reset/
    /// StartFigure/CloseFigure).
    bool new_figure{true};
};

/// A GraphicsPathIterator over a COPY of its path taken at creation (later
/// edits to the path are not seen, "iter_after_add"), plus the cursors
/// GDI+ keeps (their interplay measured by the probe's iter2 cases).
struct GpPathIterator {
    GpPath path;          ///< empty when created with a NULL path
    i32 index{0};         ///< Enumerate cursor
    i32 sub_next{0};      ///< where NextSubpath looks for the next subpath
    i32 type_start{0};    ///< subpath NextPathType walks: [type_start, type_end]
    i32 type_end{-1};
    i32 type_pos{0};      ///< NextPathType cursor
    bool type_valid{false};
    i32 marker_pos{0};    ///< NextMarker cursor
};

// ---- Primitive builders (used by GdipAddPath* and by the draw area) --------
// Each appends to @p path exactly the points/types GDI+ emits for the
// corresponding GdipAddPath* call (connecting to the open figure when one
// is open, as GDI+ does).

void path_add_lines(GpPath& path, const std::vector<PointF>& pts);
void path_add_beziers(GpPath& path, const std::vector<PointF>& pts);  ///< 3n+1 points
void path_add_arc(GpPath& path, RectF rect, float start, float sweep);
void path_add_curve(GpPath& path, const std::vector<PointF>& pts, i32 offset, i32 segments,
                    float tension);
void path_add_closed_curve(GpPath& path, const std::vector<PointF>& pts, float tension);
void path_add_rectangle(GpPath& path, RectF rect);
void path_add_ellipse(GpPath& path, RectF rect);
void path_add_pie(GpPath& path, RectF rect, float start, float sweep);
void path_add_polygon(GpPath& path, const std::vector<PointF>& pts);
void path_close_figure(GpPath& path);
void path_transform(GpPath& path, const Affine& m);

/// The path as a CoreGraphics path (curves as cubic Béziers, closed
/// subpaths closed). Caller releases with CGPathRelease.
[[nodiscard]] CGMutablePathRef path_to_cg(const GpPath& path);
/// Flatten @p in (after @p m when non-null) into line segments no further
/// than @p flatness from the curve. Types: Start/Line with Close/Marker
/// flags kept on the figure's last point.
void flatten_path(const GpPath& in, const Affine* m, float flatness, GpPath& out);
/// GdipGetPathWorldBounds without a pen: the bounds of the path's points
/// (Bézier control points included), and with @p m the bounds of that
/// rectangle's four mapped corners (as GDI+ does, so not tight under a
/// rotation/shear). Empty path: {0,0,0,0}.
[[nodiscard]] RectF path_bounds(const GpPath& path, const Affine* m);

// ---- Further shared geometry ----------------------------------------------

/// GDI+ REAL -> INT conversion (GpRound): floor(v + 0.5), so -0.5 -> 0.
[[nodiscard]] i32 gp_round(float v);
/// GdipReversePath: points reversed per the GDI+ rules (segment types move
/// with their segment, dash bits with their point, markers mirrored,
/// close flags kept on each figure's new last point).
void path_reverse(GpPath& path);
/// Whether @p pt is inside the area @p path fills (each figure implicitly
/// closed; curves flattened at kFlatnessDefault). @p winding: nonzero rule,
/// else alternate. Points exactly on an edge follow the fill convention:
/// inside on a left/top edge, outside on a right/bottom edge.
[[nodiscard]] bool path_contains_point(const GpPath& path, PointF pt, bool winding);
/// GdipWindingModeOutline: replace @p path by the outline of the area it
/// fills under the winding rule, flattened (after @p m) at @p flatness.
/// False (path untouched) when that area is empty.
bool winding_outline(GpPath& path, const Affine* m, float flatness);
/// GdipWarpPath body: flatten, map the source rectangle onto the 4-point
/// (or 3-point parallelogram) destination with @p mode (WarpModePerspective
/// 0 / WarpModeBilinear 1), then apply @p m.
void warp_path(GpPath& path, const Affine* m, const PointF* dst, i32 dst_count, RectF src,
               i32 mode, float flatness);

}  // namespace vitreo::winapi::gdip
