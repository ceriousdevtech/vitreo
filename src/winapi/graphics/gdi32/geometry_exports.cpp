// SPDX-License-Identifier: LGPL-2.1-only

/// @file geometry_exports.cpp
/// @brief gdi32 path, polygon, transform and clip exports: argument
///        marshalling with the failure contracts s3gdi_region_probe measured.
///
/// Nearly every failure here returns 0/FALSE and leaves the last error
/// untouched on Windows 11 (bad DC, too few points, no path, singular
/// transform, bad mode), so none of these handlers set one.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/memory/guest_memory.hpp"

#include <array>
#include <cstring>
#include <vector>

namespace vitreo::winapi::gdi_geometry_exports {

namespace {

using IPoint = GdiManager::IPoint;

bool read_points(Marshaler& m, GuestAddr p, u32 count, std::vector<IPoint>& out) {
    out.clear();
    if (count == 0) return true;
    if (p == 0 || count > 0x100000) return false;
    std::vector<i32> raw(static_cast<usize>(count) * 2);
    if (m.memory().read_bytes(p, reinterpret_cast<u8*>(raw.data()), raw.size() * 4).is_err()) return false;
    for (u32 i = 0; i < count; ++i) out.push_back({raw[i * 2], raw[i * 2 + 1]});
    return true;
}

bool write_points(Marshaler& m, GuestAddr p, const std::vector<IPoint>& pts) {
    std::vector<i32> raw;
    for (const auto& q : pts) {
        raw.push_back(q.first);
        raw.push_back(q.second);
    }
    return raw.empty() || m.memory().write_bytes(p, reinterpret_cast<const u8*>(raw.data()), raw.size() * 4).is_ok();
}

bool read_xform(Marshaler& m, GuestAddr p, std::array<float, 6>& out) {
    return p != 0 && m.memory().read_bytes(p, reinterpret_cast<u8*>(out.data()), 24).is_ok();
}

}  // namespace

// Polyline / Polygon / PolylineTo / PolyBezier / PolyBezierTo (hdc, lppt, c)
void poly_points(GdiManager& mgr, Marshaler& m, PolyKind kind) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr p = m.arg_ptr(1);
    const u32 count = m.arg32(2);
    std::vector<IPoint> pts;
    if (!read_points(m, p, count, pts)) {
        m.set_return(0u);
        return;
    }
    bool ok = false;
    switch (kind) {
    case PolyKind::Polyline: ok = mgr.polyline(hdc, pts); break;
    case PolyKind::PolylineTo: ok = mgr.polyline_to(hdc, pts); break;
    case PolyKind::Polygon: ok = mgr.poly_polygon(hdc, pts, {static_cast<i32>(count)}); break;
    case PolyKind::PolyBezier: ok = mgr.poly_bezier(hdc, pts, false); break;
    case PolyKind::PolyBezierTo: ok = mgr.poly_bezier(hdc, pts, true); break;
    }
    m.set_return(ok ? 1u : 0u);
}

// PolyPolygon(hdc, lppt, lpPolyCounts, nCount)
void poly_polygon(GdiManager& mgr, Marshaler& m) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr p = m.arg_ptr(1);
    const GuestAddr counts_ptr = m.arg_ptr(2);
    const u32 n = m.arg32(3);
    std::vector<i32> counts(n);
    if (n == 0 || n > 0x10000 || counts_ptr == 0 ||
        m.memory().read_bytes(counts_ptr, reinterpret_cast<u8*>(counts.data()), n * 4).is_err()) {
        m.set_return(0u);
        return;
    }
    u64 total = 0;
    for (const i32 c : counts) total += static_cast<u32>(c);
    std::vector<IPoint> pts;
    if (total > 0x100000 || !read_points(m, p, static_cast<u32>(total), pts)) {
        m.set_return(0u);
        return;
    }
    m.set_return(mgr.poly_polygon(hdc, pts, counts) ? 1u : 0u);
}

void path_op(GdiManager& mgr, Marshaler& m, PathOp op) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    bool ok = false;
    switch (op) {
    case PathOp::Begin: ok = mgr.begin_path(hdc); break;
    case PathOp::End: ok = mgr.end_path(hdc); break;
    case PathOp::Abort: ok = mgr.abort_path(hdc); break;
    case PathOp::CloseFigure: ok = mgr.close_figure(hdc); break;
    case PathOp::Fill: ok = mgr.fill_path(hdc); break;
    case PathOp::Stroke: ok = mgr.stroke_path(hdc); break;
    case PathOp::StrokeAndFill: ok = mgr.stroke_and_fill_path(hdc); break;
    case PathOp::Flatten: ok = mgr.flatten_path(hdc); break;
    case PathOp::Widen: ok = mgr.widen_path(hdc); break;
    }
    m.set_return(ok ? 1u : 0u);
}

// SelectClipPath(hdc, iMode)
void select_clip_path(GdiManager& mgr, Marshaler& m) {
    m.set_return(mgr.select_clip_path(static_cast<Handle>(m.arg(0)), static_cast<i32>(m.arg32(1))) ? 1u : 0u);
}

// PathToRegion(hdc)
void path_to_region(GdiManager& mgr, Marshaler& m) {
    m.set_return(static_cast<u64>(mgr.path_to_region(static_cast<Handle>(m.arg(0)))));
}

// GetPath(hdc, apt, aj, cpt): the point count; with buffers, cpt must hold it.
void get_path(GdiManager& mgr, Marshaler& m) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr apt = m.arg_ptr(1);
    const GuestAddr aj = m.arg_ptr(2);
    const i32 cpt = static_cast<i32>(m.arg32(3));
    std::vector<IPoint> pts;
    std::vector<u8> types;
    const i32 n = mgr.get_path(hdc, &pts, &types);
    if (n < 0) {
        m.set_return(static_cast<u32>(-1));
        return;
    }
    if (cpt == 0) {
        m.set_return(static_cast<u32>(n));
        return;
    }
    if (cpt < n || apt == 0 || aj == 0) {
        set_last_error(error_codes::ERROR_BUFFER_OVERFLOW);
        m.set_return(static_cast<u32>(-1));
        return;
    }
    (void)write_points(m, apt, pts);
    if (!types.empty()) (void)m.memory().write_bytes(aj, types.data(), types.size());
    m.set_return(static_cast<u32>(n));
}

// DPtoLP / LPtoDP(hdc, lppt, c)
void convert_points(GdiManager& mgr, Marshaler& m, bool to_logical) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr p = m.arg_ptr(1);
    const i32 count = static_cast<i32>(m.arg32(2));
    std::vector<IPoint> pts;
    if (!mgr.has_dc(hdc) || count < 0 || !read_points(m, p, static_cast<u32>(count), pts)) {
        m.set_return(0u);
        return;
    }
    const bool ok = to_logical ? mgr.dp_to_lp_points(hdc, pts) : mgr.lp_to_dp_points(hdc, pts);
    if (ok) (void)write_points(m, p, pts);
    m.set_return(ok ? 1u : 0u);
}

void set_map_mode(GdiManager& mgr, Marshaler& m) {
    m.set_return(static_cast<u32>(mgr.set_map_mode(static_cast<Handle>(m.arg(0)), static_cast<i32>(m.arg32(1)))));
}

void get_map_mode(GdiManager& mgr, Marshaler& m) {
    m.set_return(static_cast<u32>(mgr.get_map_mode(static_cast<Handle>(m.arg(0)))));
}

// SetWindowExtEx / SetViewportExtEx(hdc, x, y, lpsz)
void set_extent(GdiManager& mgr, Marshaler& m, bool viewport) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const i32 x = static_cast<i32>(m.arg32(1));
    const i32 y = static_cast<i32>(m.arg32(2));
    const GuestAddr out = m.arg_ptr(3);
    IPoint old{};
    const bool ok = viewport ? mgr.set_viewport_ext(hdc, x, y, &old) : mgr.set_window_ext(hdc, x, y, &old);
    if (ok && out != 0) (void)write_points(m, out, {old});
    m.set_return(ok ? 1u : 0u);
}

void set_graphics_mode(GdiManager& mgr, Marshaler& m) {
    m.set_return(static_cast<u32>(mgr.set_graphics_mode(static_cast<Handle>(m.arg(0)), static_cast<i32>(m.arg32(1)))));
}

void set_world_transform(GdiManager& mgr, Marshaler& m) {
    std::array<float, 6> xf{};
    if (!read_xform(m, m.arg_ptr(1), xf)) {
        m.set_return(0u);
        return;
    }
    m.set_return(mgr.set_world_transform(static_cast<Handle>(m.arg(0)), xf) ? 1u : 0u);
}

void get_world_transform(GdiManager& mgr, Marshaler& m) {
    std::array<float, 6> xf{};
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0 || !mgr.get_world_transform(static_cast<Handle>(m.arg(0)), xf)) {
        m.set_return(0u);
        return;
    }
    (void)m.memory().write_bytes(out, reinterpret_cast<const u8*>(xf.data()), 24);
    m.set_return(1u);
}

void modify_world_transform(GdiManager& mgr, Marshaler& m) {
    const i32 mode = static_cast<i32>(m.arg32(2));
    std::array<float, 6> xf{};
    const bool have = read_xform(m, m.arg_ptr(1), xf);
    m.set_return(mgr.modify_world_transform(static_cast<Handle>(m.arg(0)), have ? &xf : nullptr, mode) ? 1u : 0u);
}

// SetMiterLimit(hdc, FLOAT limit, PFLOAT old): the float arrives in XMM1 on
// x64 and as its bits on the x86 stack.
void set_miter_limit(GdiManager& mgr, Marshaler& m) {
    const float limit = m.arg_f32(1);
    const GuestAddr out = m.arg_ptr(2);
    float old = 0;
    const bool ok = mgr.set_miter_limit(static_cast<Handle>(m.arg(0)), limit, &old);
    if (ok && out != 0) (void)m.memory().write_bytes(out, reinterpret_cast<const u8*>(&old), 4);
    m.set_return(ok ? 1u : 0u);
}

void get_miter_limit(GdiManager& mgr, Marshaler& m) {
    float value = 0;
    const GuestAddr out = m.arg_ptr(1);
    const bool ok = mgr.get_miter_limit(static_cast<Handle>(m.arg(0)), value);
    if (ok && out != 0) (void)m.memory().write_bytes(out, reinterpret_cast<const u8*>(&value), 4);
    m.set_return(ok ? 1u : 0u);
}

void set_arc_direction(GdiManager& mgr, Marshaler& m) {
    m.set_return(static_cast<u32>(mgr.set_arc_direction(static_cast<Handle>(m.arg(0)), static_cast<i32>(m.arg32(1)))));
}

void get_arc_direction(GdiManager& mgr, Marshaler& m) {
    m.set_return(static_cast<u32>(mgr.get_arc_direction(static_cast<Handle>(m.arg(0)))));
}

// GetClipRgn(hdc, hrgn): 1 with the clip copied, 0 without a clip, -1 on error.
void get_clip_rgn(GdiManager& mgr, Marshaler& m) {
    m.set_return(static_cast<u32>(mgr.get_clip_region(static_cast<Handle>(m.arg(0)), static_cast<Handle>(m.arg(1)))));
}

// OffsetClipRgn(hdc, x, y)
void offset_clip_rgn(GdiManager& mgr, Marshaler& m) {
    m.set_return(static_cast<u32>(mgr.offset_clip_rgn(static_cast<Handle>(m.arg(0)), static_cast<i32>(m.arg32(1)),
                                                      static_cast<i32>(m.arg32(2)))));
}

// CreatePolygonRgn(lppt, c, mode) / CreatePolyPolygonRgn(lppt, lpc, n, mode)
void create_polygon_rgn(GdiManager& mgr, Marshaler& m, bool poly) {
    const GuestAddr p = m.arg_ptr(0);
    std::vector<i32> counts;
    i32 mode = 0;
    if (poly) {
        const GuestAddr counts_ptr = m.arg_ptr(1);
        const u32 n = m.arg32(2);
        mode = static_cast<i32>(m.arg32(3));
        counts.resize(n);
        if (n == 0 || n > 0x10000 || counts_ptr == 0 ||
            m.memory().read_bytes(counts_ptr, reinterpret_cast<u8*>(counts.data()), n * 4).is_err()) {
            m.set_return(0u);
            return;
        }
    } else {
        counts.push_back(static_cast<i32>(m.arg32(1)));
        mode = static_cast<i32>(m.arg32(2));
    }
    u64 total = 0;
    for (const i32 c : counts) {
        if (c < 2) {
            m.set_return(0u);
            return;
        }
        total += static_cast<u32>(c);
    }
    std::vector<IPoint> pts;
    if ((mode != 1 && mode != 2) || total > 0x100000 || !read_points(m, p, static_cast<u32>(total), pts)) {
        m.set_return(0u);
        return;
    }
    m.set_return(static_cast<u64>(mgr.create_polygon_region(pts, counts, mode)));
}

}  // namespace vitreo::winapi::gdi_geometry_exports
