// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_geometry.cpp
/// @brief GDI+ area: Matrix, GraphicsPath and GraphicsPathIterator exports.
///
/// Contract: GdiPlusFlat.h signatures, the Learn Matrix / GraphicsPath /
/// GraphicsPathIterator pages, and every status, default and cursor rule
/// Learn leaves open measured on Windows 11 with
/// tools/oracle_probe/gdiplus_geometry_probe.c (.x64.expected /
/// .x86.expected); quoted names are its cases. The geometry itself lives
/// in gdiplus_geometry_math.cpp. Notes: .docs/contracts/gdiplus/geometry.md.

#include "winapi/graphics/gdiplus/gdiplus_geometry.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_graphics.hpp"
#include "winapi/graphics/gdiplus/gdiplus_pen.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cmath>

namespace vitreo::winapi::gdip {

namespace {

constexpr i32 kMatrixOrderPrepend = 0;
constexpr i32 kMatrixOrderAppend = 1;

[[nodiscard]] i32 iarg(Marshaler& m, usize i) { return static_cast<i32>(m.arg32(i)); }
[[nodiscard]] float farg(Marshaler& m, usize i) { return real_arg(m, i); }
[[nodiscard]] float iarg_f(Marshaler& m, usize i) { return static_cast<float>(iarg(m, i)); }

bool write_bool(Marshaler& m, GuestAddr at, bool v) { return m.write_u32(at, v ? 1 : 0).is_ok(); }
bool write_i32(Marshaler& m, GuestAddr at, i32 v) {
    return m.write_u32(at, static_cast<u32>(v)).is_ok();
}

/// REAL points -> GDI+ `Point` with GpRound ("pointsI": -0.5 -> 0, 2.5 -> 3).
bool write_points_round(Marshaler& m, GuestAddr at, const std::vector<PointF>& pts) {
    for (usize i = 0; i < pts.size(); ++i) {
        if (!write_i32(m, at + i * 8, gp_round(pts[i].X)) ||
            !write_i32(m, at + i * 8 + 4, gp_round(pts[i].Y))) {
            return false;
        }
    }
    return true;
}

bool read_pts(Marshaler& m, GuestAddr at, i32 count, bool ints, std::vector<PointF>& out) {
    return ints ? read_points_i(m, at, count, out) : read_points(m, at, count, out);
}

template <typename F>
void reg(HandlerMap& map, const char* name, F fn) {
    map[name] = [fn](Marshaler& m) { write_status(m, fn(m)); };
}

/// The path's figure structure is what GdipCreatePath2 accepts: base types
/// Start/Line/Bezier only, flags DashMode/PathMarker/CloseSubpath only
/// ("c2_type2", "c2_bit40", "c2_bit08"), Béziers in runs of three
/// ("c2_badbez"), no single-point figure unless the path is one point
/// ("c2_trailing_start", "create2_starts", "c2_single_start").
bool types_valid(const std::vector<u8>& t) {
    const usize n = t.size();
    if (n == 1) return (t[0] & 0x48) == 0 && (t[0] & 7) != 2 && (t[0] & 7) <= 3;
    usize fig_len = 0;
    usize bez_run = 0;
    for (usize i = 0; i < n; ++i) {
        const u8 v = t[i];
        if (v & 0x48) return false;
        const u8 base = v & PathPointTypePathTypeMask;
        if (base != PathPointTypeStart && base != PathPointTypeLine && base != PathPointTypeBezier) {
            return false;
        }
        const bool start = i == 0 || base == PathPointTypeStart;
        if (start) {
            if (i != 0 && (fig_len < 2 || bez_run % 3 != 0)) return false;
            fig_len = 1;
            bez_run = 0;
            continue;
        }
        ++fig_len;
        if (base == PathPointTypeBezier) {
            ++bez_run;
        } else {
            if (bez_run % 3 != 0) return false;
            bez_run = 0;
        }
    }
    return fig_len >= 2 && bez_run % 3 == 0;
}

/// GdipCreatePath2 normalizes the first point to a plain Start
/// ("c2_line_first", "c2_marker_start", "c2_close_start").
void normalize_first(std::vector<u8>& t) {
    if (!t.empty()) t[0] = static_cast<u8>(t[0] & PathPointTypeDashMode);
}

/// Device pixels per pen-width unit for bounds (96 dpi, no graphics):
/// "bp_line_w4_inch" inflates by 384 = 4 * 96.
float pen_unit_px(i32 unit) {
    switch (unit) {
    case UnitPoint: return 96.0f / 72.0f;
    case UnitInch: return 96.0f;
    case UnitDocument: return 96.0f / 300.0f;
    case UnitMillimeter: return 96.0f / 25.4f;
    default: return 1.0f;
    }
}

/// How far GdipGetPathWorldBounds grows the bounds for @p pen, in device
/// units (not scaled by the matrix, "bp_line_w4_m3"): the pen width, times
/// MiterLimit/2 for a miter join on a path with a join (any figure of 3+
/// points, curves included: "bp_bez_w4", "bp_collinear3_w4"), times 3 for
/// an anchor cap ("bp_line_w4_caparrow"), whichever is largest.
float pen_inflation(const GpPen& pen, const GpPath& path) {
    const float w = pen.width * pen_unit_px(pen.unit);
    float factor = 1.0f;
    bool has_join = false;
    usize run = 0;
    for (usize i = 0; i < path.types.size(); ++i) {
        if (i == 0 || (path.types[i] & PathPointTypePathTypeMask) == PathPointTypeStart) run = 0;
        if (++run >= 3) has_join = true;
    }
    if (has_join && (pen.line_join == LineJoinMiter || pen.line_join == LineJoinMiterClipped)) {
        factor = std::max(factor, pen.miter_limit / 2.0f);
    }
    auto anchor = [](i32 cap) { return cap >= LineCapSquareAnchor && cap <= LineCapArrowAnchor; };
    if (anchor(pen.start_cap) || anchor(pen.end_cap)) factor = std::max(factor, 3.0f);
    return w * factor;
}

void copy_range(const GpPath& src, i32 s, i32 e, GpPath& dst) {
    dst.points.assign(src.points.begin() + s, src.points.begin() + e + 1);
    dst.types.assign(src.types.begin() + s, src.types.begin() + e + 1);
    dst.new_figure = true;
}

/// NextSubpath: the subpath from sub_next to the next Start. A start in
/// the middle of a figure (after NextMarker) is taken as is
/// ("sub_after_mark2": [4..7]).
bool iter_next_subpath(GpPathIterator& it, i32& s, i32& e) {
    const i32 n = static_cast<i32>(it.path.points.size());
    if (it.sub_next >= n) {
        it.type_valid = false;
        return false;
    }
    s = it.sub_next;
    e = s + 1;
    while (e < n && (it.path.types[static_cast<usize>(e)] & PathPointTypePathTypeMask) !=
                        PathPointTypeStart) {
        ++e;
    }
    --e;
    it.sub_next = e + 1;
    it.index = e + 1;
    it.type_start = s;
    it.type_end = e;
    it.type_pos = s;
    it.type_valid = true;
    return true;
}

/// NextMarker: the points up to and including the next marked point (or
/// the end). It also moves the Enumerate cursor to the section and the
/// subpath cursor just past its first point, and ends path-type iteration
/// ("after_mark1_enum64", "sub_after_mark2", "type2").
bool iter_next_marker(GpPathIterator& it, i32& s, i32& e) {
    const i32 n = static_cast<i32>(it.path.points.size());
    if (it.marker_pos >= n) return false;
    s = it.marker_pos;
    e = s;
    while (e < n - 1 && !(it.path.types[static_cast<usize>(e)] & PathPointTypePathMarker)) ++e;
    it.marker_pos = e + 1;
    it.index = s;
    it.sub_next = s + 1;
    it.type_valid = false;
    return true;
}

}  // namespace

void register_geometry(GdiplusManager& mgr, HandlerMap& map) {
    auto& objs = mgr.objects();
    auto path_at = [&objs](Marshaler& m, usize i) { return objs.get<GpPath>(handle_arg(m, i)); };
    auto matrix_at = [&objs](Marshaler& m, usize i) { return objs.get<GpMatrix>(handle_arg(m, i)); };
    auto iter_at = [&objs](Marshaler& m, usize i) {
        return objs.get<GpPathIterator>(handle_arg(m, i));
    };

    // ================================================================ Matrix

    reg(map, "GdipCreateMatrix", [&objs](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) return Status::InvalidParameter;
        write_handle(m, out, objs.add(GpMatrix{}));
        return Status::Ok;
    });
    reg(map, "GdipCreateMatrix2", [&objs](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(6);
        if (out == 0) return Status::InvalidParameter;
        GpMatrix mx;
        mx.m = {farg(m, 0), farg(m, 1), farg(m, 2), farg(m, 3), farg(m, 4), farg(m, 5)};
        write_handle(m, out, objs.add(mx));
        return Status::Ok;
    });
    // GdipCreateMatrix3[I](rect, dstplg[3], out): the map of the rectangle's
    // top-left, top-right, bottom-left corners onto the parallelogram. An
    // empty rectangle is OutOfMemory ("m3_w0"); a negative one is accepted.
    auto create_matrix3 = [&objs](Marshaler& m, bool ints) {
        const GuestAddr rect_at = m.arg_ptr(0), plg_at = m.arg_ptr(1), out = m.arg_ptr(2);
        if (rect_at == 0 || plg_at == 0 || out == 0) return Status::InvalidParameter;
        RectF r;
        std::vector<PointF> p;
        if (!(ints ? read_rect_i(m, rect_at, r) : read_rect(m, rect_at, r)) ||
            !read_pts(m, plg_at, 3, ints, p)) {
            return Status::InvalidParameter;
        }
        if (r.Width == 0 || r.Height == 0) return Status::OutOfMemory;
        GpMatrix mx;
        mx.m.m11 = (p[1].X - p[0].X) / r.Width;
        mx.m.m12 = (p[1].Y - p[0].Y) / r.Width;
        mx.m.m21 = (p[2].X - p[0].X) / r.Height;
        mx.m.m22 = (p[2].Y - p[0].Y) / r.Height;
        mx.m.dx = p[0].X - mx.m.m11 * r.X - mx.m.m21 * r.Y;
        mx.m.dy = p[0].Y - mx.m.m12 * r.X - mx.m.m22 * r.Y;
        write_handle(m, out, objs.add(mx));
        return Status::Ok;
    };
    reg(map, "GdipCreateMatrix3", [create_matrix3](Marshaler& m) { return create_matrix3(m, false); });
    reg(map, "GdipCreateMatrix3I", [create_matrix3](Marshaler& m) { return create_matrix3(m, true); });
    reg(map, "GdipCloneMatrix", [&objs, matrix_at](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (mx == nullptr || out == 0) return Status::InvalidParameter;
        write_handle(m, out, objs.add(GpMatrix{*mx}));
        return Status::Ok;
    });
    reg(map, "GdipDeleteMatrix", [&objs](Marshaler& m) {
        return objs.remove<GpMatrix>(handle_arg(m, 0)) ? Status::Ok : Status::InvalidParameter;
    });
    reg(map, "GdipSetMatrixElements", [matrix_at](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        if (mx == nullptr) return Status::InvalidParameter;
        mx->m = {farg(m, 1), farg(m, 2), farg(m, 3), farg(m, 4), farg(m, 5), farg(m, 6)};
        return Status::Ok;
    });
    reg(map, "GdipGetMatrixElements", [matrix_at](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (mx == nullptr || out == 0) return Status::InvalidParameter;
        const float v[6] = {mx->m.m11, mx->m.m12, mx->m.m21, mx->m.m22, mx->m.dx, mx->m.dy};
        for (int i = 0; i < 6; ++i) write_f32(m, out + 4 * static_cast<u64>(i), v[i]);
        return Status::Ok;
    });
    // Compose @p op with the matrix: Prepend applies op first (op * M),
    // Append after (M * op); any other order is InvalidParameter ("m_order2").
    auto compose = [](GpMatrix& mx, const Affine& op, i32 order) {
        if (order == kMatrixOrderPrepend) mx.m = op.then(mx.m);
        else if (order == kMatrixOrderAppend) mx.m = mx.m.then(op);
        else return Status::InvalidParameter;
        return Status::Ok;
    };
    reg(map, "GdipMultiplyMatrix", [matrix_at, compose](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        GpMatrix* other = matrix_at(m, 1);
        if (mx == nullptr || other == nullptr) return Status::InvalidParameter;
        if (mx == other) return Status::ObjectBusy;  // "m_mul_self"
        return compose(*mx, other->m, iarg(m, 2));
    });
    reg(map, "GdipTranslateMatrix", [matrix_at, compose](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        if (mx == nullptr) return Status::InvalidParameter;
        return compose(*mx, Affine{1, 0, 0, 1, farg(m, 1), farg(m, 2)}, iarg(m, 3));
    });
    reg(map, "GdipScaleMatrix", [matrix_at, compose](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        if (mx == nullptr) return Status::InvalidParameter;
        return compose(*mx, Affine{farg(m, 1), 0, 0, farg(m, 2), 0, 0}, iarg(m, 3));
    });
    // Rotation in REAL: "rotate90_exact" is m11 = cos(float(pi/2)) = 7.5e-8.
    reg(map, "GdipRotateMatrix", [matrix_at, compose](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        if (mx == nullptr) return Status::InvalidParameter;
        const float rad = farg(m, 1) * (3.14159265358979323846f / 180.0f);
        const float c = std::cos(rad), s = std::sin(rad);
        return compose(*mx, Affine{c, s, -s, c, 0, 0}, iarg(m, 2));
    });
    // Shear: [1, shearY; shearX, 1] ("shear_check").
    reg(map, "GdipShearMatrix", [matrix_at, compose](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        if (mx == nullptr) return Status::InvalidParameter;
        return compose(*mx, Affine{1, farg(m, 2), farg(m, 1), 1, 0, 0}, iarg(m, 3));
    });
    reg(map, "GdipInvertMatrix", [matrix_at](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        if (mx == nullptr) return Status::InvalidParameter;
        const Affine a = mx->m;
        const float det = a.m11 * a.m22 - a.m12 * a.m21;
        if (det == 0) return Status::InvalidParameter;  // "m_invert_singular", unchanged
        Affine r;
        r.m11 = a.m22 / det;
        r.m12 = -a.m12 / det;
        r.m21 = -a.m21 / det;
        r.m22 = a.m11 / det;
        r.dx = (a.m21 * a.dy - a.m22 * a.dx) / det;
        r.dy = (a.m12 * a.dx - a.m11 * a.dy) / det;
        mx->m = r;
        return Status::Ok;
    });
    auto transform_points = [matrix_at](Marshaler& m, bool ints, bool vector) {
        GpMatrix* mx = matrix_at(m, 0);
        const GuestAddr at = m.arg_ptr(1);
        const i32 count = iarg(m, 2);
        if (mx == nullptr || at == 0 || count <= 0) return Status::InvalidParameter;
        std::vector<PointF> pts;
        if (!read_pts(m, at, count, ints, pts)) return Status::InvalidParameter;
        Affine a = mx->m;
        if (vector) a.dx = a.dy = 0;
        for (PointF& p : pts) p = a.apply(p);
        const bool ok = ints ? write_points_round(m, at, pts) : write_points(m, at, pts);
        return ok ? Status::Ok : Status::InvalidParameter;
    };
    reg(map, "GdipTransformMatrixPoints", [transform_points](Marshaler& m) { return transform_points(m, false, false); });
    reg(map, "GdipTransformMatrixPointsI", [transform_points](Marshaler& m) { return transform_points(m, true, false); });
    reg(map, "GdipVectorTransformMatrixPoints", [transform_points](Marshaler& m) { return transform_points(m, false, true); });
    reg(map, "GdipVectorTransformMatrixPointsI", [transform_points](Marshaler& m) { return transform_points(m, true, true); });
    // Invertible: a nonzero determinant, however small ("invertible_det").
    reg(map, "GdipIsMatrixInvertible", [matrix_at](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (mx == nullptr || out == 0) return Status::InvalidParameter;
        write_bool(m, out, mx->m.m11 * mx->m.m22 - mx->m.m12 * mx->m.m21 != 0);
        return Status::Ok;
    });
    // Identity: the 2x2 part within 0.001 of the identity, the offsets
    // exactly zero ("ident_m11", "ident_m12", "ident_dx").
    reg(map, "GdipIsMatrixIdentity", [matrix_at](Marshaler& m) {
        GpMatrix* mx = matrix_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (mx == nullptr || out == 0) return Status::InvalidParameter;
        const Affine& a = mx->m;
        auto near = [](float v, float want) { return std::fabs(v - want) < 0.001f; };
        write_bool(m, out, near(a.m11, 1) && near(a.m12, 0) && near(a.m21, 0) && near(a.m22, 1) &&
                               a.dx == 0 && a.dy == 0);
        return Status::Ok;
    });
    // Equal: exact element comparison ("equal_ulp").
    reg(map, "GdipIsMatrixEqual", [matrix_at](Marshaler& m) {
        GpMatrix* a = matrix_at(m, 0);
        GpMatrix* b = matrix_at(m, 1);
        const GuestAddr out = m.arg_ptr(2);
        if (a == nullptr || b == nullptr || out == 0) return Status::InvalidParameter;
        write_bool(m, out, a->m.m11 == b->m.m11 && a->m.m12 == b->m.m12 && a->m.m21 == b->m.m21 &&
                               a->m.m22 == b->m.m22 && a->m.dx == b->m.dx && a->m.dy == b->m.dy);
        return Status::Ok;
    });

    // ================================================================= Path

    // The fill mode is stored unchecked by GdipCreatePath and
    // GdipSetPathFillMode ("create_fill2", "setfill5").
    reg(map, "GdipCreatePath", [&objs](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return Status::InvalidParameter;
        GpPath p;
        p.fill_mode = iarg(m, 0);
        write_handle(m, out, objs.add(std::move(p)));
        return Status::Ok;
    });
    // GdipCreatePath2[I](points, types, count, fill, out): count <= 0 and a
    // fill mode other than Alternate/Winding are OutOfMemory ("create2_zero",
    // "create2_fill2"); types GDI+ rejects make an EMPTY path, status Ok.
    auto create_path2 = [&objs](Marshaler& m, bool ints) {
        const GuestAddr pts_at = m.arg_ptr(0), types_at = m.arg_ptr(1), out = m.arg_ptr(4);
        const i32 count = iarg(m, 2);
        const i32 fill = iarg(m, 3);
        if (pts_at == 0 || types_at == 0 || out == 0) return Status::InvalidParameter;
        if (count <= 0 || (fill != FillModeAlternate && fill != FillModeWinding)) {
            return Status::OutOfMemory;
        }
        GpPath p;
        p.fill_mode = fill;
        std::vector<PointF> pts;
        if (!read_pts(m, pts_at, count, ints, pts)) return Status::InvalidParameter;
        std::vector<u8> types(static_cast<usize>(count));
        for (i32 i = 0; i < count; ++i) {
            const auto b = m.read_u8(types_at + static_cast<u64>(i));
            if (b.is_err()) return Status::InvalidParameter;
            types[static_cast<usize>(i)] = b.value();
        }
        normalize_first(types);
        if (types_valid(types)) {
            p.points = std::move(pts);
            p.types = std::move(types);
        }
        write_handle(m, out, objs.add(std::move(p)));
        return Status::Ok;
    };
    reg(map, "GdipCreatePath2", [create_path2](Marshaler& m) { return create_path2(m, false); });
    reg(map, "GdipCreatePath2I", [create_path2](Marshaler& m) { return create_path2(m, true); });
    reg(map, "GdipClonePath", [&objs, path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (p == nullptr || out == 0) return Status::InvalidParameter;
        write_handle(m, out, objs.add(GpPath{*p}));
        return Status::Ok;
    });
    reg(map, "GdipDeletePath", [&objs](Marshaler& m) {
        return objs.remove<GpPath>(handle_arg(m, 0)) ? Status::Ok : Status::InvalidParameter;
    });
    // Reset: empty, fill mode back to Alternate ("reset_fill").
    reg(map, "GdipResetPath", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        *p = GpPath{};
        return Status::Ok;
    });
    reg(map, "GdipGetPointCount", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (p == nullptr || out == 0) return Status::InvalidParameter;
        write_i32(m, out, static_cast<i32>(p->points.size()));
        return Status::Ok;
    });
    // Points/types: count <= 0 InvalidParameter, smaller than the path
    // InsufficientBuffer, larger accepted ("points_empty0", "points_short",
    // "points_long"); an empty path with count >= 1 is Ok.
    auto get_points = [path_at](Marshaler& m, bool ints) {
        GpPath* p = path_at(m, 0);
        const GuestAddr at = m.arg_ptr(1);
        const i32 count = iarg(m, 2);
        if (p == nullptr || at == 0 || count <= 0) return Status::InvalidParameter;
        if (static_cast<usize>(count) < p->points.size()) return Status::InsufficientBuffer;
        const bool ok = ints ? write_points_round(m, at, p->points) : write_points(m, at, p->points);
        return ok ? Status::Ok : Status::InvalidParameter;
    };
    reg(map, "GdipGetPathPoints", [get_points](Marshaler& m) { return get_points(m, false); });
    reg(map, "GdipGetPathPointsI", [get_points](Marshaler& m) { return get_points(m, true); });
    reg(map, "GdipGetPathTypes", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        const GuestAddr at = m.arg_ptr(1);
        const i32 count = iarg(m, 2);
        if (p == nullptr || at == 0 || count <= 0) return Status::InvalidParameter;
        if (static_cast<usize>(count) < p->types.size()) return Status::InsufficientBuffer;
        for (usize i = 0; i < p->types.size(); ++i) {
            if (m.write_u8(at + i, p->types[i]).is_err()) return Status::InvalidParameter;
        }
        return Status::Ok;
    });
    // GdipGetPathData(path, PathData{INT Count; PointF* Points; BYTE* Types}):
    // null buffers or Count < 0 InvalidParameter, Count short of the path
    // OutOfMemory, else the data and Count = point count ("data_short",
    // "data_long", "data_countneg", "data_empty_buf").
    reg(map, "GdipGetPathData", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        const GuestAddr data = m.arg_ptr(1);
        if (p == nullptr || data == 0) return Status::InvalidParameter;
        const u64 ps = m.cpu_is_64bit() ? 8 : 4;
        const auto count = m.read_u32(data);
        const auto pts = m.read_ptr(data + ps);
        const auto types = m.read_ptr(data + 2 * ps);
        if (count.is_err() || pts.is_err() || types.is_err()) return Status::InvalidParameter;
        if (pts.value() == 0 || types.value() == 0) return Status::InvalidParameter;
        const i32 c = static_cast<i32>(count.value());
        if (c < 0) return Status::InvalidParameter;
        if (static_cast<usize>(c) < p->points.size()) return Status::OutOfMemory;
        if (!write_points(m, pts.value(), p->points)) return Status::InvalidParameter;
        for (usize i = 0; i < p->types.size(); ++i) {
            if (m.write_u8(types.value() + i, p->types[i]).is_err()) return Status::InvalidParameter;
        }
        write_i32(m, data, static_cast<i32>(p->points.size()));
        return Status::Ok;
    });
    reg(map, "GdipGetPathFillMode", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (p == nullptr || out == 0) return Status::InvalidParameter;
        write_i32(m, out, p->fill_mode);
        return Status::Ok;
    });
    reg(map, "GdipSetPathFillMode", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        p->fill_mode = iarg(m, 1);
        return Status::Ok;
    });
    reg(map, "GdipGetPathLastPoint", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (p == nullptr || out == 0 || p->points.empty()) return Status::InvalidParameter;
        write_points(m, out, {p->points.back()});
        return Status::Ok;
    });
    reg(map, "GdipStartPathFigure", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        p->new_figure = true;
        return Status::Ok;
    });
    reg(map, "GdipClosePathFigure", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        path_close_figure(*p);
        return Status::Ok;
    });
    reg(map, "GdipClosePathFigures", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        const usize n = p->types.size();
        for (usize i = 0; i < n; ++i) {
            const bool last = i + 1 == n ||
                (p->types[i + 1] & PathPointTypePathTypeMask) == PathPointTypeStart;
            if (last) p->types[i] |= PathPointTypeCloseSubpath;
        }
        p->new_figure = true;
        return Status::Ok;
    });
    reg(map, "GdipSetPathMarker", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        if (!p->types.empty()) p->types.back() |= PathPointTypePathMarker;  // "marker_empty"
        return Status::Ok;
    });
    reg(map, "GdipClearPathMarkers", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        for (u8& t : p->types) t = static_cast<u8>(t & ~PathPointTypePathMarker);
        return Status::Ok;
    });
    reg(map, "GdipReversePath", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        path_reverse(*p);
        return Status::Ok;
    });
    // A NULL matrix is the identity ("transform_null").
    reg(map, "GdipTransformPath", [path_at, matrix_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        if (handle_arg(m, 1) != 0) {
            GpMatrix* mx = matrix_at(m, 1);
            if (mx == nullptr) return Status::InvalidParameter;
            path_transform(*p, mx->m);
        }
        return Status::Ok;
    });

    // ---- AddPath* ----
    auto add_lines = [path_at](Marshaler& m, std::vector<PointF> pts) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        path_add_lines(*p, pts);
        return Status::Ok;
    };
    reg(map, "GdipAddPathLine", [add_lines](Marshaler& m) {
        return add_lines(m, {{farg(m, 1), farg(m, 2)}, {farg(m, 3), farg(m, 4)}});
    });
    reg(map, "GdipAddPathLineI", [add_lines](Marshaler& m) {
        return add_lines(m, {{iarg_f(m, 1), iarg_f(m, 2)}, {iarg_f(m, 3), iarg_f(m, 4)}});
    });
    // Point-array builders: (path, points, count, ...). @p min is the count
    // below which GDI+ answers InvalidParameter.
    auto read_array = [path_at](Marshaler& m, bool ints, i32 min, GpPath*& p,
                                std::vector<PointF>& pts) {
        p = path_at(m, 0);
        const GuestAddr at = m.arg_ptr(1);
        const i32 count = iarg(m, 2);
        if (p == nullptr || at == 0 || count < min) return Status::InvalidParameter;
        if (!read_pts(m, at, count, ints, pts)) return Status::InvalidParameter;
        return Status::Ok;
    };
    auto line2 = [read_array](Marshaler& m, bool ints) {
        GpPath* p;
        std::vector<PointF> pts;
        // One point is a valid figure start ("line2_count1").
        if (Status s = read_array(m, ints, 1, p, pts); s != Status::Ok) return s;
        path_add_lines(*p, pts);
        return Status::Ok;
    };
    reg(map, "GdipAddPathLine2", [line2](Marshaler& m) { return line2(m, false); });
    reg(map, "GdipAddPathLine2I", [line2](Marshaler& m) { return line2(m, true); });

    auto arc = [path_at](Marshaler& m, RectF r) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        if (!(r.Width > 0) || !(r.Height > 0)) return Status::InvalidParameter;  // "arc(0,0,0,50..)"
        path_add_arc(*p, r, farg(m, 5), farg(m, 6));
        return Status::Ok;
    };
    reg(map, "GdipAddPathArc", [arc](Marshaler& m) {
        return arc(m, {farg(m, 1), farg(m, 2), farg(m, 3), farg(m, 4)});
    });
    reg(map, "GdipAddPathArcI", [arc](Marshaler& m) {
        return arc(m, {iarg_f(m, 1), iarg_f(m, 2), iarg_f(m, 3), iarg_f(m, 4)});
    });
    auto bezier = [path_at](Marshaler& m, bool ints) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        std::vector<PointF> pts(4);
        for (usize i = 0; i < 4; ++i) {
            pts[i] = ints ? PointF{iarg_f(m, 1 + 2 * i), iarg_f(m, 2 + 2 * i)}
                          : PointF{farg(m, 1 + 2 * i), farg(m, 2 + 2 * i)};
        }
        path_add_beziers(*p, pts);
        return Status::Ok;
    };
    reg(map, "GdipAddPathBezier", [bezier](Marshaler& m) { return bezier(m, false); });
    reg(map, "GdipAddPathBezierI", [bezier](Marshaler& m) { return bezier(m, true); });
    auto beziers = [read_array](Marshaler& m, bool ints) {
        GpPath* p;
        std::vector<PointF> pts;
        if (Status s = read_array(m, ints, 4, p, pts); s != Status::Ok) return s;
        if ((pts.size() - 1) % 3 != 0) return Status::InvalidParameter;  // "beziers5"
        path_add_beziers(*p, pts);
        return Status::Ok;
    };
    reg(map, "GdipAddPathBeziers", [beziers](Marshaler& m) { return beziers(m, false); });
    reg(map, "GdipAddPathBeziersI", [beziers](Marshaler& m) { return beziers(m, true); });

    // Curves: tension 0.5 unless given; Curve3 needs offset >= 0,
    // segments >= 1 and offset + segments <= count - 1 ("curve3_o0s5",
    // "curve3_o0s0", "curve3_on1s1").
    auto curve = [read_array](Marshaler& m, bool ints, int kind) {
        GpPath* p;
        std::vector<PointF> pts;
        if (Status s = read_array(m, ints, 2, p, pts); s != Status::Ok) return s;
        const i32 n = static_cast<i32>(pts.size());
        i32 offset = 0, segments = n - 1;
        float tension = 0.5f;
        if (kind == 2) tension = farg(m, 3);
        if (kind == 3) {
            offset = iarg(m, 3);
            segments = iarg(m, 4);
            tension = farg(m, 5);
            if (offset < 0 || segments < 1 || offset + segments > n - 1) {
                return Status::InvalidParameter;
            }
        }
        path_add_curve(*p, pts, offset, segments, tension);
        return Status::Ok;
    };
    reg(map, "GdipAddPathCurve", [curve](Marshaler& m) { return curve(m, false, 1); });
    reg(map, "GdipAddPathCurveI", [curve](Marshaler& m) { return curve(m, true, 1); });
    reg(map, "GdipAddPathCurve2", [curve](Marshaler& m) { return curve(m, false, 2); });
    reg(map, "GdipAddPathCurve2I", [curve](Marshaler& m) { return curve(m, true, 2); });
    reg(map, "GdipAddPathCurve3", [curve](Marshaler& m) { return curve(m, false, 3); });
    reg(map, "GdipAddPathCurve3I", [curve](Marshaler& m) { return curve(m, true, 3); });
    auto closed_curve = [read_array](Marshaler& m, bool ints, bool tension) {
        GpPath* p;
        std::vector<PointF> pts;
        if (Status s = read_array(m, ints, 3, p, pts); s != Status::Ok) return s;  // "closed2"
        path_add_closed_curve(*p, pts, tension ? farg(m, 3) : 0.5f);
        return Status::Ok;
    };
    reg(map, "GdipAddPathClosedCurve", [closed_curve](Marshaler& m) { return closed_curve(m, false, false); });
    reg(map, "GdipAddPathClosedCurveI", [closed_curve](Marshaler& m) { return closed_curve(m, true, false); });
    reg(map, "GdipAddPathClosedCurve2", [closed_curve](Marshaler& m) { return closed_curve(m, false, true); });
    reg(map, "GdipAddPathClosedCurve2I", [closed_curve](Marshaler& m) { return closed_curve(m, true, true); });

    auto ellipse = [path_at](Marshaler& m, RectF r) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        path_add_ellipse(*p, r);
        return Status::Ok;
    };
    reg(map, "GdipAddPathEllipse", [ellipse](Marshaler& m) {
        return ellipse(m, {farg(m, 1), farg(m, 2), farg(m, 3), farg(m, 4)});
    });
    reg(map, "GdipAddPathEllipseI", [ellipse](Marshaler& m) {
        return ellipse(m, {iarg_f(m, 1), iarg_f(m, 2), iarg_f(m, 3), iarg_f(m, 4)});
    });
    auto pie = [path_at](Marshaler& m, RectF r) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        path_add_pie(*p, r, farg(m, 5), farg(m, 6));
        return (r.Width > 0 && r.Height > 0) ? Status::Ok : Status::InvalidParameter;
    };
    reg(map, "GdipAddPathPie", [pie](Marshaler& m) {
        return pie(m, {farg(m, 1), farg(m, 2), farg(m, 3), farg(m, 4)});
    });
    reg(map, "GdipAddPathPieI", [pie](Marshaler& m) {
        return pie(m, {iarg_f(m, 1), iarg_f(m, 2), iarg_f(m, 3), iarg_f(m, 4)});
    });
    auto polygon = [read_array](Marshaler& m, bool ints) {
        GpPath* p;
        std::vector<PointF> pts;
        if (Status s = read_array(m, ints, 3, p, pts); s != Status::Ok) return s;  // "poly2"
        path_add_polygon(*p, pts);
        return Status::Ok;
    };
    reg(map, "GdipAddPathPolygon", [polygon](Marshaler& m) { return polygon(m, false); });
    reg(map, "GdipAddPathPolygonI", [polygon](Marshaler& m) { return polygon(m, true); });
    auto rectangle = [path_at](Marshaler& m, RectF r) {
        GpPath* p = path_at(m, 0);
        if (p == nullptr) return Status::InvalidParameter;
        path_add_rectangle(*p, r);
        return Status::Ok;
    };
    reg(map, "GdipAddPathRectangle", [rectangle](Marshaler& m) {
        return rectangle(m, {farg(m, 1), farg(m, 2), farg(m, 3), farg(m, 4)});
    });
    reg(map, "GdipAddPathRectangleI", [rectangle](Marshaler& m) {
        return rectangle(m, {iarg_f(m, 1), iarg_f(m, 2), iarg_f(m, 3), iarg_f(m, 4)});
    });
    // Rectangles: count <= 0 or NULL InvalidParameter; empty ones skipped
    // ("rects3", "rects0", "rects_onlyempty").
    auto rectangles = [path_at](Marshaler& m, bool ints) {
        GpPath* p = path_at(m, 0);
        const GuestAddr at = m.arg_ptr(1);
        const i32 count = iarg(m, 2);
        if (p == nullptr || at == 0 || count <= 0) return Status::InvalidParameter;
        std::vector<RectF> rs;
        if (!(ints ? read_rects_i(m, at, count, rs) : read_rects(m, at, count, rs))) {
            return Status::InvalidParameter;
        }
        for (const RectF& r : rs) path_add_rectangle(*p, r);
        return Status::Ok;
    };
    reg(map, "GdipAddPathRectangles", [rectangles](Marshaler& m) { return rectangles(m, false); });
    reg(map, "GdipAddPathRectanglesI", [rectangles](Marshaler& m) { return rectangles(m, true); });

    // AddPathPath: an empty source is InvalidParameter ("addpath_empty").
    // With connect, the source's first figure joins the open figure unless
    // that source figure is closed ("addpath_connect_closed_rect"), its
    // first point dropped when it repeats the last ("addpath_connect_bez").
    reg(map, "GdipAddPathPath", [path_at](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        GpPath* add = path_at(m, 1);
        const bool connect = m.arg32(2) != 0;
        if (p == nullptr || add == nullptr || add->points.empty()) return Status::InvalidParameter;
        const GpPath src = *add;  // the source may be the path itself ("addpath_self")
        usize first_end = 1;
        while (first_end < src.types.size() &&
               (src.types[first_end] & PathPointTypePathTypeMask) != PathPointTypeStart) {
            ++first_end;
        }
        const bool first_closed = (src.types[first_end - 1] & PathPointTypeCloseSubpath) != 0;
        usize i = 0;
        if (connect && !p->new_figure && !p->points.empty() && !first_closed) {
            const u8 flags = static_cast<u8>(src.types[0] & ~PathPointTypePathTypeMask);
            if (!(p->points.back().X == src.points[0].X && p->points.back().Y == src.points[0].Y)) {
                p->points.push_back(src.points[0]);
                p->types.push_back(static_cast<u8>(PathPointTypeLine | flags));
            } else if (flags != 0) {
                p->types.back() |= flags;
            }
            i = 1;
        }
        for (; i < src.points.size(); ++i) {
            p->points.push_back(src.points[i]);
            p->types.push_back(src.types[i]);
        }
        p->new_figure = (p->types.back() & PathPointTypeCloseSubpath) != 0;
        return Status::Ok;
    });

    // ---- Flatten / widen / outline / warp ----
    auto optional_matrix = [matrix_at](Marshaler& m, usize i, const Affine*& out, bool& bad) {
        out = nullptr;
        bad = false;
        if (handle_arg(m, i) == 0) return;
        GpMatrix* mx = matrix_at(m, i);
        if (mx == nullptr) bad = true;
        else out = &mx->m;
    };
    reg(map, "GdipFlattenPath", [path_at, optional_matrix](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        const Affine* mx;
        bool bad;
        optional_matrix(m, 1, mx, bad);
        if (p == nullptr || bad) return Status::InvalidParameter;
        GpPath out;
        flatten_path(*p, mx, farg(m, 2), out);
        *p = std::move(out);
        return Status::Ok;
    });
    // WidenPath: NULL pen InvalidParameter, an empty path OutOfMemory
    // ("widen_nullpen", "widen_empty"); the outline is the draw area's.
    reg(map, "GdipWidenPath", [&objs, path_at, optional_matrix](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        GpPen* pen = objs.get<GpPen>(handle_arg(m, 1));
        const Affine* mx;
        bool bad;
        optional_matrix(m, 2, mx, bad);
        if (p == nullptr || pen == nullptr || bad) return Status::InvalidParameter;
        if (p->points.empty()) return Status::OutOfMemory;
        GpPath out;
        widen_path(*p, *pen, mx, farg(m, 3), out);
        out.fill_mode = p->fill_mode;
        out.new_figure = true;
        *p = std::move(out);
        return Status::Ok;
    });
    // WindingModeOutline: an empty path is Ok; a path enclosing no area is
    // InvalidParameter and left as it was ("winding_open_line").
    reg(map, "GdipWindingModeOutline", [path_at, optional_matrix](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        const Affine* mx;
        bool bad;
        optional_matrix(m, 1, mx, bad);
        if (p == nullptr || bad) return Status::InvalidParameter;
        if (p->points.empty()) return Status::Ok;
        return winding_outline(*p, mx, farg(m, 2)) ? Status::Ok : Status::InvalidParameter;
    });
    // WarpPath(path, matrix, points, count, srcx, srcy, srcw, srch, mode,
    // flatness): count <= 0 / NULL points InvalidParameter ("warp_count0");
    // any count but 4 uses three points; an unknown mode empties the path
    // ("warp_mode2").
    reg(map, "GdipWarpPath", [path_at, optional_matrix](Marshaler& m) {
        GpPath* p = path_at(m, 0);
        const Affine* mx;
        bool bad;
        optional_matrix(m, 1, mx, bad);
        const GuestAddr at = m.arg_ptr(2);
        const i32 count = iarg(m, 3);
        if (p == nullptr || bad || at == 0 || count <= 0) return Status::InvalidParameter;
        const RectF src{farg(m, 4), farg(m, 5), farg(m, 6), farg(m, 7)};
        const i32 mode = iarg(m, 8);
        const float flatness = farg(m, 9);
        std::vector<PointF> dst;
        if (!read_points(m, at, count == 4 ? 4 : 3, dst)) return Status::InvalidParameter;
        if (mode != 0 && mode != 1) {
            p->points.clear();
            p->types.clear();
            p->new_figure = true;
            return Status::Ok;
        }
        if (p->points.empty()) return Status::Ok;
        warp_path(*p, mx, dst.data(), count == 4 ? 4 : 3, src, mode, flatness);
        return Status::Ok;
    });

    // ---- Bounds / hit test ----
    auto world_bounds = [&objs, path_at, optional_matrix](Marshaler& m, RectF& out) {
        GpPath* p = path_at(m, 0);
        const Affine* mx;
        bool bad;
        optional_matrix(m, 2, mx, bad);
        GpPen* pen = nullptr;
        if (handle_arg(m, 3) != 0) {
            pen = objs.get<GpPen>(handle_arg(m, 3));
            if (pen == nullptr) bad = true;
        }
        if (p == nullptr || bad || m.arg_ptr(1) == 0) return Status::InvalidParameter;
        out = path_bounds(*p, mx);
        if (pen != nullptr && !p->points.empty()) {
            const float d = pen_inflation(*pen, *p);
            out = {out.X - d, out.Y - d, out.Width + 2 * d, out.Height + 2 * d};
        }
        return Status::Ok;
    };
    reg(map, "GdipGetPathWorldBounds", [world_bounds](Marshaler& m) {
        RectF r;
        const Status s = world_bounds(m, r);
        if (s == Status::Ok) write_rect(m, m.arg_ptr(1), r);
        return s;
    });
    // INT bounds cover every pixel touched: floor of the near edges, the far
    // edges rounded up plus one ("boundsI_frac": 0.4..10.2 -> X 0, W 12).
    reg(map, "GdipGetPathWorldBoundsI", [world_bounds](Marshaler& m) {
        RectF r;
        const Status s = world_bounds(m, r);
        if (s != Status::Ok) return s;
        const GuestAddr out = m.arg_ptr(1);
        if (r.Width == 0 && r.Height == 0 && r.X == 0 && r.Y == 0) {
            for (int i = 0; i < 4; ++i) write_i32(m, out + 4 * static_cast<u64>(i), 0);
            return s;
        }
        const i32 x = static_cast<i32>(std::floor(r.X));
        const i32 y = static_cast<i32>(std::floor(r.Y));
        const i32 w = static_cast<i32>(std::ceil(r.X + r.Width)) - x + 1;
        const i32 h = static_cast<i32>(std::ceil(r.Y + r.Height)) - y + 1;
        write_i32(m, out, x);
        write_i32(m, out + 4, y);
        write_i32(m, out + 8, w);
        write_i32(m, out + 12, h);
        return s;
    });
    // IsOutlineVisiblePathPoint(path, x, y, pen, graphics, result): the
    // point goes to device space (graphics transform, identity without one),
    // is rounded to its pixel, and that pixel centre is tested against the
    // widened outline with the fill convention ("ov_w3", "ov_w4", "ov_w4_x").
    // A UnitWorld pen widens in world space and scales with the transform
    // ("ov_world_pen_gx4"); other units are device sizes ("ov_pixel_pen_gx4").
    // A zero-width pen hits nothing ("ov_w0").
    auto outline_visible = [&objs, path_at](Marshaler& m, PointF pt) {
        GpPath* p = path_at(m, 0);
        GpPen* pen = objs.get<GpPen>(handle_arg(m, 3));
        const u64 gh = handle_arg(m, 4);
        GpGraphics* g = gh != 0 ? objs.get<GpGraphics>(gh) : nullptr;
        const GuestAddr out = m.arg_ptr(5);
        if (p == nullptr || pen == nullptr || out == 0 || (gh != 0 && g == nullptr)) {
            return Status::InvalidParameter;
        }
        bool hit = false;
        if (!p->points.empty() && pen->width != 0) {
            const Affine dev = g != nullptr ? world_to_device(*g) : Affine{};
            GpPath outline;
            if (pen->unit == UnitWorld) {
                widen_path(*p, *pen, nullptr, kFlatnessDefault, outline);
                path_transform(outline, dev);
            } else {
                widen_path(*p, *pen, &dev, kFlatnessDefault, outline);
            }
            const PointF d = dev.apply(pt);
            hit = path_contains_point(
                outline, {static_cast<float>(gp_round(d.X)), static_cast<float>(gp_round(d.Y))},
                true);
        }
        write_bool(m, out, hit);
        return Status::Ok;
    };
    reg(map, "GdipIsOutlineVisiblePathPoint", [outline_visible](Marshaler& m) {
        return outline_visible(m, {farg(m, 1), farg(m, 2)});
    });
    reg(map, "GdipIsOutlineVisiblePathPointI", [outline_visible](Marshaler& m) {
        return outline_visible(m, {iarg_f(m, 1), iarg_f(m, 2)});
    });

    // ========================================================= PathIterator

    // A NULL path makes an iterator over nothing ("iter_null").
    reg(map, "GdipCreatePathIter", [&objs, path_at](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) return Status::InvalidParameter;
        GpPathIterator it;
        if (handle_arg(m, 1) != 0) {
            GpPath* p = path_at(m, 1);
            if (p == nullptr) return Status::InvalidParameter;
            it.path = *p;
        }
        write_handle(m, out, objs.add(std::move(it)));
        return Status::Ok;
    });
    reg(map, "GdipDeletePathIter", [&objs](Marshaler& m) {
        return objs.remove<GpPathIterator>(handle_arg(m, 0)) ? Status::Ok : Status::InvalidParameter;
    });
    // NextSubpath(it, count, start, end, closed). Past the end of a
    // non-empty path it reports 0 with start = end = 0 and closed TRUE
    // ("iter_markers": sub rc=0 s=0 e=0 closed=1); an empty one writes only
    // the count ("iter_empty").
    reg(map, "GdipPathIterNextSubpath", [iter_at](Marshaler& m) {
        GpPathIterator* it = iter_at(m, 0);
        const GuestAddr rc = m.arg_ptr(1), s_at = m.arg_ptr(2), e_at = m.arg_ptr(3),
                        c_at = m.arg_ptr(4);
        if (it == nullptr || rc == 0 || s_at == 0 || e_at == 0 || c_at == 0) {
            return Status::InvalidParameter;
        }
        i32 s = 0, e = 0;
        if (!iter_next_subpath(*it, s, e)) {
            write_i32(m, rc, 0);
            if (!it->path.points.empty()) {
                write_i32(m, s_at, 0);
                write_i32(m, e_at, 0);
                write_bool(m, c_at, true);
            }
            return Status::Ok;
        }
        write_i32(m, rc, e - s + 1);
        write_i32(m, s_at, s);
        write_i32(m, e_at, e);
        write_bool(m, c_at, (it->path.types[static_cast<usize>(e)] & PathPointTypeCloseSubpath) != 0);
        return Status::Ok;
    });
    // NextSubpathPath(it, count, path, closed): the subpath copied into
    // @p path (its fill mode kept, "subpathpath_into_nonempty"); past the
    // end the path is left alone. A NULL path is accepted.
    reg(map, "GdipPathIterNextSubpathPath", [iter_at, path_at](Marshaler& m) {
        GpPathIterator* it = iter_at(m, 0);
        const GuestAddr rc = m.arg_ptr(1), c_at = m.arg_ptr(3);
        GpPath* target = handle_arg(m, 2) != 0 ? path_at(m, 2) : nullptr;
        if (it == nullptr || rc == 0 || c_at == 0 || (handle_arg(m, 2) != 0 && target == nullptr)) {
            return Status::InvalidParameter;
        }
        i32 s = 0, e = 0;
        if (!iter_next_subpath(*it, s, e)) {
            write_i32(m, rc, 0);
            if (!it->path.points.empty()) write_bool(m, c_at, true);
            return Status::Ok;
        }
        if (target != nullptr) copy_range(it->path, s, e, *target);
        write_i32(m, rc, e - s + 1);
        write_bool(m, c_at, (it->path.types[static_cast<usize>(e)] & PathPointTypeCloseSubpath) != 0);
        return Status::Ok;
    });
    // NextPathType(it, count, type, start, end): runs of one segment type
    // inside the subpath NextSubpath returned, sharing their boundary point
    // ("iter_mixed": 03 [0..3] then 01 [3..5]); nothing before a subpath or
    // after a NextMarker ("iter_type_before_sub", "type2").
    reg(map, "GdipPathIterNextPathType", [iter_at](Marshaler& m) {
        GpPathIterator* it = iter_at(m, 0);
        const GuestAddr rc = m.arg_ptr(1), t_at = m.arg_ptr(2), s_at = m.arg_ptr(3),
                        e_at = m.arg_ptr(4);
        if (it == nullptr || rc == 0 || t_at == 0 || s_at == 0 || e_at == 0) {
            return Status::InvalidParameter;
        }
        if (!it->type_valid || it->type_pos >= it->type_end) {
            write_i32(m, rc, 0);
            return Status::Ok;
        }
        const auto& types = it->path.types;
        const i32 s = it->type_pos;
        const u8 kind = types[static_cast<usize>(s + 1)] & PathPointTypePathTypeMask;
        i32 e = s + 1;
        while (e < it->type_end &&
               (types[static_cast<usize>(e + 1)] & PathPointTypePathTypeMask) == kind) {
            ++e;
        }
        it->type_pos = e;
        write_i32(m, rc, e - s + 1);
        (void)m.write_u8(t_at, kind);
        write_i32(m, s_at, s);
        write_i32(m, e_at, e);
        return Status::Ok;
    });
    reg(map, "GdipPathIterNextMarker", [iter_at](Marshaler& m) {
        GpPathIterator* it = iter_at(m, 0);
        const GuestAddr rc = m.arg_ptr(1), s_at = m.arg_ptr(2), e_at = m.arg_ptr(3);
        if (it == nullptr || rc == 0 || s_at == 0 || e_at == 0) return Status::InvalidParameter;
        i32 s = 0, e = 0;
        if (!iter_next_marker(*it, s, e)) {
            write_i32(m, rc, 0);
            return Status::Ok;
        }
        write_i32(m, rc, e - s + 1);
        write_i32(m, s_at, s);
        write_i32(m, e_at, e);
        return Status::Ok;
    });
    reg(map, "GdipPathIterNextMarkerPath", [iter_at, path_at](Marshaler& m) {
        GpPathIterator* it = iter_at(m, 0);
        const GuestAddr rc = m.arg_ptr(1);
        GpPath* target = handle_arg(m, 2) != 0 ? path_at(m, 2) : nullptr;
        if (it == nullptr || rc == 0 || (handle_arg(m, 2) != 0 && target == nullptr)) {
            return Status::InvalidParameter;
        }
        i32 s = 0, e = 0;
        if (!iter_next_marker(*it, s, e)) {
            write_i32(m, rc, 0);
            return Status::Ok;
        }
        if (target != nullptr) copy_range(it->path, s, e, *target);
        write_i32(m, rc, e - s + 1);
        return Status::Ok;
    });
    auto iter_count = [iter_at](Marshaler& m, int what) {
        GpPathIterator* it = iter_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (it == nullptr || out == 0) return Status::InvalidParameter;
        const auto& t = it->path.types;
        i32 v = 0;
        if (what == 0) {
            v = static_cast<i32>(t.size());
        } else if (what == 1) {
            for (u8 b : t) v += (b & PathPointTypePathTypeMask) == PathPointTypeStart ? 1 : 0;
        } else if (what == 2) {
            for (u8 b : t) v |= (b & PathPointTypePathTypeMask) == PathPointTypeBezier ? 1 : 0;
        } else {
            v = 1;
        }
        if (what >= 2) write_bool(m, out, v != 0);
        else write_i32(m, out, v);
        return Status::Ok;
    };
    reg(map, "GdipPathIterGetCount", [iter_count](Marshaler& m) { return iter_count(m, 0); });
    reg(map, "GdipPathIterGetSubpathCount", [iter_count](Marshaler& m) { return iter_count(m, 1); });
    reg(map, "GdipPathIterHasCurve", [iter_count](Marshaler& m) { return iter_count(m, 2); });
    reg(map, "GdipPathIterIsValid", [iter_count](Marshaler& m) { return iter_count(m, 3); });
    reg(map, "GdipPathIterRewind", [iter_at](Marshaler& m) {
        GpPathIterator* it = iter_at(m, 0);
        if (it == nullptr) return Status::InvalidParameter;
        GpPath keep = std::move(it->path);
        *it = GpPathIterator{};
        it->path = std::move(keep);
        return Status::Ok;
    });
    auto copy_out = [](Marshaler& m, const GpPath& p, i32 s, i32 count, GuestAddr pts,
                       GuestAddr types) {
        for (i32 i = 0; i < count; ++i) {
            const usize k = static_cast<usize>(s + i);
            if (!write_f32(m, pts + static_cast<u64>(i) * 8, p.points[k].X) ||
                !write_f32(m, pts + static_cast<u64>(i) * 8 + 4, p.points[k].Y) ||
                m.write_u8(types + static_cast<u64>(i), p.types[k]).is_err()) {
                return false;
            }
        }
        return true;
    };
    // Enumerate(it, count, points, types, n): up to n points from the
    // Enumerate cursor, which it advances along with the subpath cursor
    // ("fresh_enum2b", "after_sub1_enum64" then "sub2 rc=0").
    reg(map, "GdipPathIterEnumerate", [iter_at, copy_out](Marshaler& m) {
        GpPathIterator* it = iter_at(m, 0);
        const GuestAddr rc = m.arg_ptr(1), pts = m.arg_ptr(2), types = m.arg_ptr(3);
        const i32 want = iarg(m, 4);
        if (it == nullptr || rc == 0 || pts == 0 || types == 0) return Status::InvalidParameter;
        const i32 n = static_cast<i32>(it->path.points.size());
        const i32 got = want <= 0 ? 0 : std::max(0, std::min(want, n - it->index));
        if (got > 0 && !copy_out(m, it->path, it->index, got, pts, types)) {
            return Status::InvalidParameter;
        }
        it->index += got;
        if (got > 0) it->sub_next = it->index;
        write_i32(m, rc, got);
        return Status::Ok;
    });
    // CopyData(it, count, points, types, start, end): the inclusive range,
    // or 0 points when it is not inside the path ("copy(4,2)", "copy(0,1000)").
    reg(map, "GdipPathIterCopyData", [iter_at, copy_out](Marshaler& m) {
        GpPathIterator* it = iter_at(m, 0);
        const GuestAddr rc = m.arg_ptr(1), pts = m.arg_ptr(2), types = m.arg_ptr(3);
        const i32 s = iarg(m, 4), e = iarg(m, 5);
        if (it == nullptr || rc == 0 || pts == 0 || types == 0) return Status::InvalidParameter;
        const i32 n = static_cast<i32>(it->path.points.size());
        if (s < 0 || e >= n || s > e) {
            write_i32(m, rc, 0);
            return Status::Ok;
        }
        if (!copy_out(m, it->path, s, e - s + 1, pts, types)) return Status::InvalidParameter;
        write_i32(m, rc, e - s + 1);
        return Status::Ok;
    });
}

}  // namespace vitreo::winapi::gdip
