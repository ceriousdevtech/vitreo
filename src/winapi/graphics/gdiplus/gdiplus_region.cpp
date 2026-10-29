// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_region.cpp
/// @brief GDI+ area: Region exports and the graphics clip.
///
/// Contract: GdiPlusFlat.h signatures, the Learn Region / Graphics pages,
/// and tools/oracle_probe/gdiplus_region_probe.c (.expected) for every
/// status, getter value and raster rule (see gdiplus_region.hpp and
/// .docs/contracts/gdiplus/region.md).

#include "winapi/graphics/gdiplus/gdiplus_region.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_geometry.hpp"
#include "winapi/graphics/gdiplus/gdiplus_image.hpp"
#include "winapi/graphics/gdiplus/gdiplus_pen_brush.hpp"
#include "winapi/graphics/gdiplus/gdiplus_render.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>

namespace vitreo::winapi::gdip {

namespace {

void ret(Marshaler& m, Status s) { write_status(m, s); }

/// GpRound: floor(v + 0.5) (probe "pixeloffset half clipboundsI").
i32 px_round(double v) {
    const double r = std::floor(v + 0.5);
    if (r <= -2147483648.0) return INT32_MIN;
    if (r >= 2147483647.0) return INT32_MAX;
    return static_cast<i32>(r);
}

bool write_bool(Marshaler& m, GuestAddr at, bool v) { return m.write_u32(at, v ? 1u : 0u).is_ok(); }

bool write_rect_round(Marshaler& m, GuestAddr at, const RectF& r) {
    return m.write_u32(at, static_cast<u32>(px_round(r.X))).is_ok() &&
           m.write_u32(at + 4, static_cast<u32>(px_round(r.Y))).is_ok() &&
           m.write_u32(at + 8, static_cast<u32>(px_round(r.Width))).is_ok() &&
           m.write_u32(at + 12, static_cast<u32>(px_round(r.Height))).is_ok();
}

bool valid_mode(i32 mode) { return mode >= CombineModeReplace && mode <= CombineModeComplement; }

bool invert(const Affine& a, Affine& out) {
    const double det = double(a.m11) * a.m22 - double(a.m12) * a.m21;
    if (det == 0 || !std::isfinite(det)) return false;
    const double i11 = a.m22 / det, i12 = -a.m12 / det, i21 = -a.m21 / det, i22 = a.m11 / det;
    out.m11 = static_cast<float>(i11);
    out.m12 = static_cast<float>(i12);
    out.m21 = static_cast<float>(i21);
    out.m22 = static_cast<float>(i22);
    out.dx = static_cast<float>(-(a.dx * i11 + a.dy * i21));
    out.dy = static_cast<float>(-(a.dx * i12 + a.dy * i22));
    return true;
}

Affine device_of(const GpGraphics* g) { return g != nullptr ? world_to_device(*g) : Affine{}; }

/// The bounding box, in world units, of device pixels [x0,x1) x [y0,y1).
RectF device_box_to_world(const GpGraphics* g, i32 x0, i32 y0, i32 x1, i32 y1) {
    Affine inv;
    if (!invert(device_of(g), inv)) return {};
    const double xs[2] = {double(x0), double(x1)}, ys[2] = {double(y0), double(y1)};
    double lx = 1e300, ly = 1e300, hx = -1e300, hy = -1e300;
    for (double x : xs) {
        for (double y : ys) {
            const double wx = inv.m11 * x + inv.m21 * y + inv.dx;
            const double wy = inv.m12 * x + inv.m22 * y + inv.dy;
            lx = std::min(lx, wx); hx = std::max(hx, wx);
            ly = std::min(ly, wy); hy = std::max(hy, wy);
        }
    }
    return {static_cast<float>(lx), static_cast<float>(ly), static_cast<float>(hx - lx),
            static_cast<float>(hy - ly)};
}

/// Raster bounds (empty: 0,0,0,0) mapped back to world.
RectF scans_world_bounds(const GpGraphics* g, const RegionScans& s) {
    i32 x0, y0, x1, y1;
    if (!s.bounds(x0, y0, x1, y1)) return {};
    return device_box_to_world(g, x0, y0, x1, y1);
}

/// GetRegionBounds: a lone leaf reports its own geometry in world units
/// (probe "rect", "negrect", "ellipse", "world2 rect"); anything combined
/// reports its device raster's bounds mapped back ("rp.intersect",
/// "fracunion world2", "inf-exclude world2").
RectF region_bounds(const GpRegion& r, const GpGraphics* g) {
    switch (r.kind) {
    case GpRegion::Kind::Rect: return r.rect;
    case GpRegion::Kind::Empty: return {};
    case GpRegion::Kind::Infinite:
        return {float(kRegionInfiniteMin), float(kRegionInfiniteMin),
                float(kRegionInfiniteMax - kRegionInfiniteMin), float(kRegionInfiniteMax - kRegionInfiniteMin)};
    case GpRegion::Kind::Path: {
        if (r.path.points.empty()) return {};
        float lx = r.path.points[0].X, hx = lx, ly = r.path.points[0].Y, hy = ly;
        for (const PointF& p : r.path.points) {
            lx = std::min(lx, p.X); hx = std::max(hx, p.X);
            ly = std::min(ly, p.Y); hy = std::max(hy, p.Y);
        }
        return {lx, ly, hx - lx, hy - ly};
    }
    case GpRegion::Kind::Combine: return scans_world_bounds(g, region_raster(r, device_of(g)));
    }
    return {};
}

/// Pixel a world point lands on.
void device_pixel(const GpGraphics* g, float x, float y, i32& px, i32& py) {
    const Affine d = device_of(g);
    px = px_round(double(d.m11) * x + double(d.m21) * y + d.dx);
    py = px_round(double(d.m12) * x + double(d.m22) * y + d.dy);
}

/// The pixels a graphics may draw to: its image's bounds, or the DC
/// surface inside the DC clip (probe "hdc dcclip visclip").
RegionScans target_scans(GdiplusManager& mgr, const GpGraphics& g) {
    if (g.kind == TargetKind::Image) {
        const GpImage* image = mgr.objects().get<GpImage>(g.image);
        if (image == nullptr) return {};
        return RegionScans::rect(0, 0, image->width, image->height);
    }
    GdiManager* gdi = mgr.gdi();
    if (gdi == nullptr) return {};
    RegionScans out;
    gdi->with_device_surface(static_cast<Handle>(g.hdc), [&](GdiManager::DeviceSurface& s) {
        out = RegionScans::rect(0, 0, s.width, s.height);
        if (s.clip != nullptr && s.clip->valid) {
            RegionScans clip;
            for (const auto& q : s.clip->parts())
                clip = scans_combine(clip, RegionScans::rect(q[0], q[1], q[2], q[3]), CombineModeUnion);
            out = scans_combine(out, clip, CombineModeIntersect);
        }
    });
    return out;
}

RegionScans clip_scans(const GraphicsState& s) {
    RegionScans out = s.clip ? region_device_scans(*s.clip) : RegionScans::infinite();
    if (s.container_clip) out = scans_combine(out, region_device_scans(*s.container_clip), CombineModeIntersect);
    return out;
}

RegionScans visible_scans(GdiplusManager& mgr, const GpGraphics& g) {
    return scans_combine(clip_scans(g.state), target_scans(mgr, g), CombineModeIntersect);
}

/// Combine @p operand (device space) into @p g's clip. The clip object may
/// be shared with a saved state, so it is replaced, never edited.
void combine_clip(GpGraphics& g, const GpRegion& operand, i32 mode) {
    auto next = g.state.clip ? region_clone(*g.state.clip) : std::make_shared<GpRegion>();
    region_combine(*next, operand, mode);
    g.state.clip = next->kind == GpRegion::Kind::Infinite ? nullptr : next;
}

/// The clip operand of SetClipRect[I]: a zero extent is the empty region
/// (probe "zero"), a negative one is normalized ("negclip").
GpRegion clip_rect_operand(const GpGraphics& g, RectF r) {
    GpRegion op;
    if (r.Width < 0) { r.X += r.Width; r.Width = -r.Width; }
    if (r.Height < 0) { r.Y += r.Height; r.Height = -r.Height; }
    if (r.Width == 0 || r.Height == 0) {
        op.kind = GpRegion::Kind::Empty;
        return op;
    }
    op.kind = GpRegion::Kind::Rect;
    op.rect = r;
    region_transform(op, world_to_device(g));
    return op;
}

GpRegion region_of_path(const GpPath& p) {
    GpRegion r;
    r.kind = GpRegion::Kind::Path;
    r.path = p;
    return r;
}

}  // namespace

void register_region(GdiplusManager& mgr, HandlerMap& map) {
    auto& objs = mgr.objects();
    auto region_at = [&objs](Marshaler& m, usize i) { return objs.get<GpRegion>(handle_arg(m, i)); };
    auto graphics_at = [&objs](Marshaler& m, usize i) { return objs.get<GpGraphics>(handle_arg(m, i)); };

    // ---- Lifetime ----------------------------------------------------------

    auto add_region = [&objs](Marshaler& m, GuestAddr out, std::shared_ptr<GpRegion> r) {
        const u64 h = objs.add(std::move(r));
        if (!write_handle(m, out, h)) {
            objs.remove<GpRegion>(h);
            return ret(m, Status::InvalidParameter);
        }
        ret(m, Status::Ok);
    };

    map["GdipCreateRegion"] = [add_region](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) return ret(m, Status::InvalidParameter);
        add_region(m, out, std::make_shared<GpRegion>());
    };
    map["GdipCreateRegionRect"] = [add_region](Marshaler& m) {
        const GuestAddr rect = m.arg_ptr(0), out = m.arg_ptr(1);
        auto r = std::make_shared<GpRegion>();
        r->kind = GpRegion::Kind::Rect;
        if (rect == 0 || out == 0 || !read_rect(m, rect, r->rect)) return ret(m, Status::InvalidParameter);
        add_region(m, out, std::move(r));
    };
    map["GdipCreateRegionRectI"] = [add_region](Marshaler& m) {
        const GuestAddr rect = m.arg_ptr(0), out = m.arg_ptr(1);
        auto r = std::make_shared<GpRegion>();
        r->kind = GpRegion::Kind::Rect;
        if (rect == 0 || out == 0 || !read_rect_i(m, rect, r->rect)) return ret(m, Status::InvalidParameter);
        add_region(m, out, std::move(r));
    };
    map["GdipCreateRegionPath"] = [&objs, add_region](Marshaler& m) {
        const GpPath* path = objs.get<GpPath>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (path == nullptr || out == 0) return ret(m, Status::InvalidParameter);
        add_region(m, out, std::make_shared<GpRegion>(region_of_path(*path)));
    };
    map["GdipCreateRegionRgnData"] = [add_region](Marshaler& m) {
        const GuestAddr data = m.arg_ptr(0);
        const i32 size = static_cast<i32>(m.arg32(1));
        const GuestAddr out = m.arg_ptr(2);
        if (data == 0 || out == 0) return ret(m, Status::InvalidParameter);
        // GDI+ answers GenericError for every malformed stream (probe
        // "err rgndata(*)", "rgndata *").
        if (size < 16 || size > (64 << 20)) return ret(m, Status::GenericError);
        std::vector<u8> bytes(static_cast<usize>(size));
        if (m.memory().read_bytes(data, bytes.data(), bytes.size()).is_err())
            return ret(m, Status::InvalidParameter);
        auto r = std::make_shared<GpRegion>();
        if (!region_deserialize(bytes.data(), bytes.size(), *r)) return ret(m, Status::GenericError);
        add_region(m, out, std::move(r));
    };
    map["GdipCreateRegionHrgn"] = [&mgr, add_region](Marshaler& m) {
        const Handle hrgn = static_cast<Handle>(m.arg(0));
        const GuestAddr out = m.arg_ptr(1);
        ClipRect shape;
        if (hrgn == 0 || out == 0 || mgr.gdi() == nullptr || !mgr.gdi()->get_region_rect(hrgn, shape))
            return ret(m, Status::InvalidParameter);
        add_region(m, out, std::make_shared<GpRegion>(region_from_rects(shape.parts())));
    };
    map["GdipCloneRegion"] = [region_at, add_region](Marshaler& m) {
        const GpRegion* r = region_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (r == nullptr || out == 0) return ret(m, Status::InvalidParameter);
        add_region(m, out, region_clone(*r));
    };
    map["GdipDeleteRegion"] = [&objs](Marshaler& m) {
        ret(m, objs.remove<GpRegion>(handle_arg(m, 0)) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipSetInfinite"] = [region_at](Marshaler& m) {
        GpRegion* r = region_at(m, 0);
        if (r == nullptr) return ret(m, Status::InvalidParameter);
        *r = GpRegion{};
        ret(m, Status::Ok);
    };
    map["GdipSetEmpty"] = [region_at](Marshaler& m) {
        GpRegion* r = region_at(m, 0);
        if (r == nullptr) return ret(m, Status::InvalidParameter);
        *r = GpRegion{};
        r->kind = GpRegion::Kind::Empty;
        ret(m, Status::Ok);
    };

    // ---- Combine / transform ----------------------------------------------

    auto combine_rect = [region_at](Marshaler& m, bool integer) {
        GpRegion* r = region_at(m, 0);
        const GuestAddr at = m.arg_ptr(1);
        const i32 mode = static_cast<i32>(m.arg32(2));
        GpRegion op;
        op.kind = GpRegion::Kind::Rect;
        if (r == nullptr || at == 0 || !valid_mode(mode) ||
            !(integer ? read_rect_i(m, at, op.rect) : read_rect(m, at, op.rect)))
            return ret(m, Status::InvalidParameter);
        region_combine(*r, op, mode);
        ret(m, Status::Ok);
    };
    map["GdipCombineRegionRect"] = [combine_rect](Marshaler& m) { combine_rect(m, false); };
    map["GdipCombineRegionRectI"] = [combine_rect](Marshaler& m) { combine_rect(m, true); };
    map["GdipCombineRegionPath"] = [&objs, region_at](Marshaler& m) {
        GpRegion* r = region_at(m, 0);
        const GpPath* path = objs.get<GpPath>(handle_arg(m, 1));
        const i32 mode = static_cast<i32>(m.arg32(2));
        if (r == nullptr || path == nullptr || !valid_mode(mode)) return ret(m, Status::InvalidParameter);
        region_combine(*r, region_of_path(*path), mode);
        ret(m, Status::Ok);
    };
    map["GdipCombineRegionRegion"] = [region_at](Marshaler& m) {
        GpRegion* r = region_at(m, 0);
        const GpRegion* r2 = region_at(m, 1);
        const i32 mode = static_cast<i32>(m.arg32(2));
        if (r == nullptr || r2 == nullptr) return ret(m, Status::InvalidParameter);
        // A region combined with itself is locked twice: ObjectBusy, ahead
        // of the mode check (probe "err combineregion(self)", "mode 8").
        if (r == r2) return ret(m, Status::ObjectBusy);
        if (!valid_mode(mode)) return ret(m, Status::InvalidParameter);
        region_combine(*r, *r2, mode);
        ret(m, Status::Ok);
    };
    map["GdipTranslateRegion"] = [region_at](Marshaler& m) {
        GpRegion* r = region_at(m, 0);
        if (r == nullptr) return ret(m, Status::InvalidParameter);
        region_translate(*r, real_arg(m, 1), real_arg(m, 2));
        ret(m, Status::Ok);
    };
    map["GdipTranslateRegionI"] = [region_at](Marshaler& m) {
        GpRegion* r = region_at(m, 0);
        if (r == nullptr) return ret(m, Status::InvalidParameter);
        region_translate(*r, static_cast<float>(static_cast<i32>(m.arg32(1))),
                         static_cast<float>(static_cast<i32>(m.arg32(2))));
        ret(m, Status::Ok);
    };
    map["GdipTransformRegion"] = [&objs, region_at](Marshaler& m) {
        GpRegion* r = region_at(m, 0);
        const GpMatrix* mx = objs.get<GpMatrix>(handle_arg(m, 1));
        if (r == nullptr || mx == nullptr) return ret(m, Status::InvalidParameter);
        region_transform(*r, mx->m);
        ret(m, Status::Ok);
    };

    // ---- Queries -----------------------------------------------------------

    map["GdipGetRegionBounds"] = [region_at, graphics_at](Marshaler& m) {
        const GpRegion* r = region_at(m, 0);
        const GpGraphics* g = graphics_at(m, 1);
        const GuestAddr out = m.arg_ptr(2);
        if (r == nullptr || g == nullptr || out == 0) return ret(m, Status::InvalidParameter);
        ret(m, write_rect(m, out, region_bounds(*r, g)) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipGetRegionBoundsI"] = [region_at, graphics_at](Marshaler& m) {
        const GpRegion* r = region_at(m, 0);
        const GpGraphics* g = graphics_at(m, 1);
        const GuestAddr out = m.arg_ptr(2);
        if (r == nullptr || g == nullptr || out == 0) return ret(m, Status::InvalidParameter);
        ret(m, write_rect_round(m, out, region_bounds(*r, g)) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipIsEmptyRegion"] = [region_at, graphics_at](Marshaler& m) {
        const GpRegion* r = region_at(m, 0);
        const GpGraphics* g = graphics_at(m, 1);
        const GuestAddr out = m.arg_ptr(2);
        if (r == nullptr || g == nullptr || out == 0) return ret(m, Status::InvalidParameter);
        bool empty = r->kind == GpRegion::Kind::Empty;
        if (r->kind != GpRegion::Kind::Empty && r->kind != GpRegion::Kind::Infinite)
            empty = region_raster(*r, world_to_device(*g)).empty();
        ret(m, write_bool(m, out, empty) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipIsInfiniteRegion"] = [region_at, graphics_at](Marshaler& m) {
        const GpRegion* r = region_at(m, 0);
        const GpGraphics* g = graphics_at(m, 1);
        const GuestAddr out = m.arg_ptr(2);
        if (r == nullptr || g == nullptr || out == 0) return ret(m, Status::InvalidParameter);
        // Pixel-based: a rect covering the infinite square is infinite
        // (probe "infrect"); an infinite tree with a hole is not ("inf.xor").
        bool inf = r->kind == GpRegion::Kind::Infinite;
        if (r->kind != GpRegion::Kind::Infinite && r->kind != GpRegion::Kind::Empty)
            inf = region_raster(*r, world_to_device(*g)).is_infinite();
        ret(m, write_bool(m, out, inf) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipIsEqualRegion"] = [region_at, graphics_at](Marshaler& m) {
        const GpRegion* a = region_at(m, 0);
        const GpRegion* b = region_at(m, 1);
        const GpGraphics* g = graphics_at(m, 2);
        const GuestAddr out = m.arg_ptr(3);
        if (a == nullptr || b == nullptr || g == nullptr || out == 0) return ret(m, Status::InvalidParameter);
        const Affine d = world_to_device(*g);
        ret(m, write_bool(m, out, region_raster(*a, d) == region_raster(*b, d)) ? Status::Ok
                                                                               : Status::InvalidParameter);
    };
    map["GdipGetRegionDataSize"] = [region_at](Marshaler& m) {
        const GpRegion* r = region_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (r == nullptr || out == 0) return ret(m, Status::InvalidParameter);
        const u32 n = static_cast<u32>(region_serialize(*r).size());
        ret(m, m.write_u32(out, n).is_ok() ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipGetRegionData"] = [region_at](Marshaler& m) {
        const GpRegion* r = region_at(m, 0);
        const GuestAddr buf = m.arg_ptr(1);
        const u32 cap = m.arg32(2);
        const GuestAddr filled = m.arg_ptr(3);
        if (r == nullptr || buf == 0 || cap == 0) return ret(m, Status::InvalidParameter);
        std::vector<u8> data = region_serialize(*r);
        if (cap < data.size()) {
            // GDI+ streams whole fields until the buffer is full, then
            // patches a size of what it wrote and no checksum (probe
            // "partial cap=*"): InsufficientBuffer, sizeFilled = bufferSize.
            if (cap >= 8) {
                const usize body = (static_cast<usize>(cap) - 8) / 4 * 4;
                std::vector<u8> part(data.begin(), data.begin() + 8 + static_cast<long>(body));
                const u32 n = cap - 8;
                for (int i = 0; i < 4; ++i) { part[i] = static_cast<u8>(n >> (8 * i)); part[4 + i] = 0; }
                if (m.memory().write_bytes(buf, part.data(), part.size()).is_err())
                    return ret(m, Status::InvalidParameter);
            }
            if (filled != 0) (void)m.write_u32(filled, cap);
            return ret(m, Status::InsufficientBuffer);
        }
        if (m.memory().write_bytes(buf, data.data(), data.size()).is_err())
            return ret(m, Status::InvalidParameter);
        if (filled != 0) (void)m.write_u32(filled, static_cast<u32>(data.size()));
        ret(m, Status::Ok);
    };
    map["GdipGetRegionHRgn"] = [&mgr, region_at, graphics_at](Marshaler& m) {
        const GpRegion* r = region_at(m, 0);
        const u64 gh = handle_arg(m, 1);
        const GpGraphics* g = graphics_at(m, 1);
        const GuestAddr out = m.arg_ptr(2);
        if (r == nullptr || out == 0 || (gh != 0 && g == nullptr)) return ret(m, Status::InvalidParameter);
        // Learn: an infinite region yields NULL.
        if (r->kind == GpRegion::Kind::Infinite)
            return ret(m, write_handle(m, out, 0) ? Status::Ok : Status::InvalidParameter);
        GdiManager* gdi = mgr.gdi();
        if (gdi == nullptr) return ret(m, Status::GenericError);
        const RegionScans s = region_raster(*r, device_of(g));
        const Handle h = gdi->create_rect_rgn(0, 0, 0, 0);
        if (!s.empty()) {
            ClipRect shape;
            s.bounds(shape.left, shape.top, shape.right, shape.bottom);
            if (s.rect_count() > 1) {
                for (const ScanBand& b : s.bands)
                    for (usize i = 0; i + 1 < b.xs.size(); i += 2)
                        shape.rectangles.push_back({b.xs[i], b.y0, b.xs[i + 1], b.y1});
            }
            gdi->set_region(h, shape);
        }
        ret(m, write_handle(m, out, h) ? Status::Ok : Status::InvalidParameter);
    };

    auto region_point = [region_at, graphics_at](Marshaler& m, bool integer) {
        const GpRegion* r = region_at(m, 0);
        const float x = integer ? float(static_cast<i32>(m.arg32(1))) : real_arg(m, 1);
        const float y = integer ? float(static_cast<i32>(m.arg32(2))) : real_arg(m, 2);
        const u64 gh = handle_arg(m, 3);
        const GpGraphics* g = graphics_at(m, 3);
        const GuestAddr out = m.arg_ptr(4);
        if (r == nullptr || out == 0 || (gh != 0 && g == nullptr)) return ret(m, Status::InvalidParameter);
        i32 px, py;
        device_pixel(g, x, y, px, py);
        const bool in = region_raster(*r, device_of(g)).contains(px, py);
        ret(m, write_bool(m, out, in) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipIsVisibleRegionPoint"] = [region_point](Marshaler& m) { region_point(m, false); };
    map["GdipIsVisibleRegionPointI"] = [region_point](Marshaler& m) { region_point(m, true); };

    auto region_rect = [region_at, graphics_at](Marshaler& m, bool integer) {
        const GpRegion* r = region_at(m, 0);
        RectF q;
        if (integer) {
            q = {float(static_cast<i32>(m.arg32(1))), float(static_cast<i32>(m.arg32(2))),
                 float(static_cast<i32>(m.arg32(3))), float(static_cast<i32>(m.arg32(4)))};
        } else {
            q = {real_arg(m, 1), real_arg(m, 2), real_arg(m, 3), real_arg(m, 4)};
        }
        const u64 gh = handle_arg(m, 5);
        const GpGraphics* g = graphics_at(m, 5);
        const GuestAddr out = m.arg_ptr(6);
        if (r == nullptr || out == 0 || (gh != 0 && g == nullptr)) return ret(m, Status::InvalidParameter);
        const Affine d = device_of(g);
        const bool hit = !scans_combine(region_raster(*r, d), rect_raster(q, d), CombineModeIntersect).empty();
        ret(m, write_bool(m, out, hit) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipIsVisibleRegionRect"] = [region_rect](Marshaler& m) { region_rect(m, false); };
    map["GdipIsVisibleRegionRectI"] = [region_rect](Marshaler& m) { region_rect(m, true); };

    map["GdipGetRegionScansCount"] = [&objs, region_at](Marshaler& m) {
        const GpRegion* r = region_at(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        const GpMatrix* mx = objs.get<GpMatrix>(handle_arg(m, 2));
        if (r == nullptr || out == 0 || mx == nullptr) return ret(m, Status::InvalidParameter);
        const u32 n = static_cast<u32>(region_raster(*r, mx->m).rect_count());
        ret(m, m.write_u32(out, n).is_ok() ? Status::Ok : Status::InvalidParameter);
    };
    auto scans = [&objs, region_at](Marshaler& m, bool integer) {
        const GpRegion* r = region_at(m, 0);
        const GuestAddr rects = m.arg_ptr(1);
        const GuestAddr count = m.arg_ptr(2);
        const GpMatrix* mx = objs.get<GpMatrix>(handle_arg(m, 3));
        if (r == nullptr || count == 0 || mx == nullptr) return ret(m, Status::InvalidParameter);
        const RegionScans s = region_raster(*r, mx->m);
        // A NULL rects buffer reports the count only (probe "err scans(NULL rects)").
        if (rects != 0) {
            GuestAddr at = rects;
            for (const ScanBand& b : s.bands) {
                for (usize i = 0; i + 1 < b.xs.size(); i += 2, at += 16) {
                    const i32 v[4] = {b.xs[i], b.y0, b.xs[i + 1] - b.xs[i], b.y1 - b.y0};
                    for (int k = 0; k < 4; ++k) {
                        const bool ok = integer ? m.write_u32(at + 4 * k, static_cast<u32>(v[k])).is_ok()
                                                : write_f32(m, at + 4 * k, static_cast<float>(v[k]));
                        if (!ok) return ret(m, Status::InvalidParameter);
                    }
                }
            }
        }
        ret(m, m.write_u32(count, static_cast<u32>(s.rect_count())).is_ok() ? Status::Ok
                                                                           : Status::InvalidParameter);
    };
    map["GdipGetRegionScans"] = [scans](Marshaler& m) { scans(m, false); };
    map["GdipGetRegionScansI"] = [scans](Marshaler& m) { scans(m, true); };

    map["GdipFillRegion"] = [&mgr, &objs, region_at, graphics_at](Marshaler& m) {
        GpGraphics* g = graphics_at(m, 0);
        const GpBrush* brush = objs.get<GpBrush>(handle_arg(m, 1));
        const GpRegion* r = region_at(m, 2);
        if (g == nullptr || brush == nullptr || r == nullptr) return ret(m, Status::InvalidParameter);
        if (g->hdc_out) return ret(m, Status::ObjectBusy);
        RenderScope scope(mgr, m, *g);
        if (scope.status() != Status::Ok || scope.cg() == nullptr) return ret(m, scope.status());
        // GDI+ fills a region's pixels exactly, antialiasing or not (probe
        // "fill rect.5 aa"): the device raster, clipped to the canvas,
        // drawn as world-space rectangles so the brush keeps its mapping.
        const Affine d = world_to_device(*g);
        Affine inv;
        if (!invert(d, inv)) return ret(m, Status::Ok);
        const RegionScans s = scans_combine(region_raster(*r, d),
                                            RegionScans::rect(0, 0, scope.width(), scope.height()),
                                            CombineModeIntersect);
        if (s.empty()) return ret(m, scope.commit());
        CGMutablePathRef path = CGPathCreateMutable();
        for (const ScanBand& b : s.bands) {
            for (usize i = 0; i + 1 < b.xs.size(); i += 2) {
                // Pixel i is centred on device coordinate i, its square is
                // [i - 0.5, i + 0.5). The aliased CoreGraphics fill also takes
                // a pixel the shape only grazes, so each span is drawn 0.1 px
                // inside its pixel squares: exactly the raster's pixels.
                const float x0 = float(b.xs[i]) - 0.4f, x1 = float(b.xs[i + 1]) - 0.6f;
                const float y0 = float(b.y0) - 0.4f, y1 = float(b.y1) - 0.6f;
                const PointF c[4] = {inv.apply({x0, y0}), inv.apply({x1, y0}), inv.apply({x1, y1}),
                                     inv.apply({x0, y1})};
                CGPathMoveToPoint(path, nullptr, c[0].X, c[0].Y);
                for (int k = 1; k < 4; ++k) CGPathAddLineToPoint(path, nullptr, c[k].X, c[k].Y);
                CGPathCloseSubpath(path);
            }
        }
        CGContextRef cg = scope.cg();
        CGContextSaveGState(cg);
        CGContextSetShouldAntialias(cg, false);
        Status st = fill_path(scope, path, false, *brush);
        CGContextRestoreGState(cg);
        CGPathRelease(path);
        const Status cs = scope.commit();
        ret(m, st != Status::Ok ? st : cs);
    };

    map["GdipIsVisiblePathPoint"] = [&objs, graphics_at](Marshaler& m) {
        const GpPath* path = objs.get<GpPath>(handle_arg(m, 0));
        const float x = real_arg(m, 1), y = real_arg(m, 2);
        const u64 gh = handle_arg(m, 3);
        const GpGraphics* g = graphics_at(m, 3);
        const GuestAddr out = m.arg_ptr(4);
        if (path == nullptr || out == 0 || (gh != 0 && g == nullptr)) return ret(m, Status::InvalidParameter);
        // Raster of the path's fill in device space; the graphics clip does
        // not apply (probe "pathvis clipped").
        i32 px, py;
        device_pixel(g, x, y, px, py);
        const bool in = path_raster(*path, device_of(g)).contains(px, py);
        ret(m, write_bool(m, out, in) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipIsVisiblePathPointI"] = [&objs, graphics_at](Marshaler& m) {
        const GpPath* path = objs.get<GpPath>(handle_arg(m, 0));
        const float x = float(static_cast<i32>(m.arg32(1))), y = float(static_cast<i32>(m.arg32(2)));
        const u64 gh = handle_arg(m, 3);
        const GpGraphics* g = graphics_at(m, 3);
        const GuestAddr out = m.arg_ptr(4);
        if (path == nullptr || out == 0 || (gh != 0 && g == nullptr)) return ret(m, Status::InvalidParameter);
        i32 px, py;
        device_pixel(g, x, y, px, py);
        const bool in = path_raster(*path, device_of(g)).contains(px, py);
        ret(m, write_bool(m, out, in) ? Status::Ok : Status::InvalidParameter);
    };

    // ---- Graphics clip -----------------------------------------------------
    // GraphicsState::clip is device space; setters transform their operand
    // by world_to_device, getters map back (probe "rect then world2").

    auto busy_graphics = [graphics_at](Marshaler& m, GpGraphics*& g) {
        g = graphics_at(m, 0);
        if (g == nullptr) { ret(m, Status::InvalidParameter); return false; }
        if (g->hdc_out) { ret(m, Status::ObjectBusy); return false; }
        return true;
    };

    map["GdipSetClipGraphics"] = [busy_graphics, graphics_at](Marshaler& m) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const GpGraphics* src = graphics_at(m, 1);
        const i32 mode = static_cast<i32>(m.arg32(2));
        if (src == nullptr || !valid_mode(mode)) return ret(m, Status::InvalidParameter);
        // The source's device clip, as is (probe "from g2 world2").
        const GpRegion op = src->state.clip ? *region_clone(*src->state.clip) : GpRegion{};
        combine_clip(*g, op, mode);
        ret(m, Status::Ok);
    };
    map["GdipSetClipHrgn"] = [&mgr, busy_graphics](Marshaler& m) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const Handle hrgn = static_cast<Handle>(m.arg(1));
        const i32 mode = static_cast<i32>(m.arg32(2));
        ClipRect shape;
        if (hrgn == 0 || !valid_mode(mode) || mgr.gdi() == nullptr || !mgr.gdi()->get_region_rect(hrgn, shape))
            return ret(m, Status::InvalidParameter);
        // An HRGN is in device units already (probe "hrgn world2").
        combine_clip(*g, region_from_rects(shape.parts()), mode);
        ret(m, Status::Ok);
    };
    map["GdipSetClipPath"] = [&objs, busy_graphics](Marshaler& m) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const GpPath* path = objs.get<GpPath>(handle_arg(m, 1));
        const i32 mode = static_cast<i32>(m.arg32(2));
        if (path == nullptr || !valid_mode(mode)) return ret(m, Status::InvalidParameter);
        GpRegion op = region_of_path(*path);
        region_transform(op, world_to_device(*g));
        combine_clip(*g, op, mode);
        ret(m, Status::Ok);
    };
    map["GdipSetClipRect"] = [busy_graphics](Marshaler& m) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const RectF r{real_arg(m, 1), real_arg(m, 2), real_arg(m, 3), real_arg(m, 4)};
        const i32 mode = static_cast<i32>(m.arg32(5));
        if (!valid_mode(mode)) return ret(m, Status::InvalidParameter);
        combine_clip(*g, clip_rect_operand(*g, r), mode);
        ret(m, Status::Ok);
    };
    map["GdipSetClipRectI"] = [busy_graphics](Marshaler& m) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const RectF r{float(static_cast<i32>(m.arg32(1))), float(static_cast<i32>(m.arg32(2))),
                      float(static_cast<i32>(m.arg32(3))), float(static_cast<i32>(m.arg32(4)))};
        const i32 mode = static_cast<i32>(m.arg32(5));
        if (!valid_mode(mode)) return ret(m, Status::InvalidParameter);
        combine_clip(*g, clip_rect_operand(*g, r), mode);
        ret(m, Status::Ok);
    };
    map["GdipSetClipRegion"] = [busy_graphics, region_at](Marshaler& m) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const GpRegion* r = region_at(m, 1);
        const i32 mode = static_cast<i32>(m.arg32(2));
        if (r == nullptr || !valid_mode(mode)) return ret(m, Status::InvalidParameter);
        auto op = region_clone(*r);
        region_transform(*op, world_to_device(*g));
        combine_clip(*g, *op, mode);
        ret(m, Status::Ok);
    };
    map["GdipResetClip"] = [busy_graphics](Marshaler& m) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        g->state.clip.reset();
        ret(m, Status::Ok);
    };
    auto translate_clip = [busy_graphics](Marshaler& m, bool integer) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const float dx = integer ? float(static_cast<i32>(m.arg32(1))) : real_arg(m, 1);
        const float dy = integer ? float(static_cast<i32>(m.arg32(2))) : real_arg(m, 2);
        if (g->state.clip) {
            // The offset is in world units: its device image is the linear
            // part of world_to_device (probe "translate(5,5) world2").
            const Affine d = world_to_device(*g);
            auto next = region_clone(*g->state.clip);
            region_translate(*next, d.m11 * dx + d.m21 * dy, d.m12 * dx + d.m22 * dy);
            g->state.clip = next;
        }
        ret(m, Status::Ok);
    };
    map["GdipTranslateClip"] = [translate_clip](Marshaler& m) { translate_clip(m, false); };
    map["GdipTranslateClipI"] = [translate_clip](Marshaler& m) { translate_clip(m, true); };
    map["GdipGetClip"] = [busy_graphics, region_at](Marshaler& m) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        GpRegion* r = region_at(m, 1);
        if (r == nullptr) return ret(m, Status::InvalidParameter);
        if (!g->state.clip) {
            *r = GpRegion{};
            return ret(m, Status::Ok);
        }
        auto world = region_clone(*g->state.clip);
        Affine inv;
        if (invert(world_to_device(*g), inv)) region_transform(*world, inv);
        *r = std::move(*world);
        ret(m, Status::Ok);
    };
    auto clip_bounds = [busy_graphics](Marshaler& m, bool integer) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return ret(m, Status::InvalidParameter);
        // The clip's raster bounds in world units, even for a lone rect
        // (probe "frac": 10.3,10.6,20.2,20.2 reports 11,11,20,20).
        const RegionScans s = g->state.clip ? region_device_scans(*g->state.clip) : RegionScans::infinite();
        const RectF b = scans_world_bounds(g, s);
        ret(m, (integer ? write_rect_round(m, out, b) : write_rect(m, out, b)) ? Status::Ok
                                                                              : Status::InvalidParameter);
    };
    map["GdipGetClipBounds"] = [clip_bounds](Marshaler& m) { clip_bounds(m, false); };
    map["GdipGetClipBoundsI"] = [clip_bounds](Marshaler& m) { clip_bounds(m, true); };
    auto visible_bounds = [&mgr, busy_graphics](Marshaler& m, bool integer) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return ret(m, Status::InvalidParameter);
        const RectF b = scans_world_bounds(g, visible_scans(mgr, *g));
        ret(m, (integer ? write_rect_round(m, out, b) : write_rect(m, out, b)) ? Status::Ok
                                                                              : Status::InvalidParameter);
    };
    map["GdipGetVisibleClipBounds"] = [visible_bounds](Marshaler& m) { visible_bounds(m, false); };
    map["GdipGetVisibleClipBoundsI"] = [visible_bounds](Marshaler& m) { visible_bounds(m, true); };
    map["GdipIsClipEmpty"] = [busy_graphics](Marshaler& m) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return ret(m, Status::InvalidParameter);
        const bool empty = g->state.clip && region_device_scans(*g->state.clip).empty();
        ret(m, write_bool(m, out, empty) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipIsVisibleClipEmpty"] = [&mgr, busy_graphics](Marshaler& m) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return ret(m, Status::InvalidParameter);
        ret(m, write_bool(m, out, visible_scans(mgr, *g).empty()) ? Status::Ok : Status::InvalidParameter);
    };
    auto visible_point = [&mgr, busy_graphics](Marshaler& m, bool integer) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        const float x = integer ? float(static_cast<i32>(m.arg32(1))) : real_arg(m, 1);
        const float y = integer ? float(static_cast<i32>(m.arg32(2))) : real_arg(m, 2);
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0) return ret(m, Status::InvalidParameter);
        i32 px, py;
        device_pixel(g, x, y, px, py);
        ret(m, write_bool(m, out, visible_scans(mgr, *g).contains(px, py)) ? Status::Ok
                                                                          : Status::InvalidParameter);
    };
    map["GdipIsVisiblePoint"] = [visible_point](Marshaler& m) { visible_point(m, false); };
    map["GdipIsVisiblePointI"] = [visible_point](Marshaler& m) { visible_point(m, true); };
    auto visible_rect = [&mgr, busy_graphics](Marshaler& m, bool integer) {
        GpGraphics* g;
        if (!busy_graphics(m, g)) return;
        RectF q;
        if (integer) {
            q = {float(static_cast<i32>(m.arg32(1))), float(static_cast<i32>(m.arg32(2))),
                 float(static_cast<i32>(m.arg32(3))), float(static_cast<i32>(m.arg32(4)))};
        } else {
            q = {real_arg(m, 1), real_arg(m, 2), real_arg(m, 3), real_arg(m, 4)};
        }
        const GuestAddr out = m.arg_ptr(5);
        if (out == 0) return ret(m, Status::InvalidParameter);
        const bool hit = !scans_combine(visible_scans(mgr, *g), rect_raster(q, world_to_device(*g)),
                                        CombineModeIntersect)
                              .empty();
        ret(m, write_bool(m, out, hit) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipIsVisibleRect"] = [visible_rect](Marshaler& m) { visible_rect(m, false); };
    map["GdipIsVisibleRectI"] = [visible_rect](Marshaler& m) { visible_rect(m, true); };
}

}  // namespace vitreo::winapi::gdip
