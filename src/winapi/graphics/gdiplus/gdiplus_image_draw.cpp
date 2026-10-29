// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_image_draw.cpp
/// @brief GDI+ DrawImage* family, DrawImageFX, cached bitmaps, and
/// GdipGetImageGraphicsContext.
///
/// Every DrawImage variant reduces to one operation: a source rectangle
/// (in the source unit, converted to image pixels with the image's
/// resolution) mapped onto a world-space parallelogram (top-left,
/// top-right, bottom-left). The source pixels in that rectangle are read
/// as straight ARGB, run through the ImageAttributes pipeline (Bitmap
/// category), extended past the image edge by the attributes' wrap mode
/// (default: clamp to transparent), premultiplied, and drawn with
/// CoreGraphics into the RenderScope with the graphics' interpolation,
/// compositing mode and clip.
///
/// Measured rules (tools/oracle_probe/gdiplus_image_probe.c "draw"):
///  * DrawImage(x, y) and DrawImagePointRect size the destination from the
///    image's physical size (pixels * graphics dpi / image dpi);
///  * DrawImagePoints takes exactly 3 points: 4 is NotImplemented, other
///    counts InvalidParameter;
///  * a source unit of UnitWorld or UnitDisplay is InvalidParameter;
///  * a locked source bitmap is WrongState;
///  * the abort callback is asked once before drawing; TRUE answers Aborted
///    with nothing drawn;
///  * an image pixel covers whole device pixels whatever the
///    PixelOffsetMode (a 1:1 draw at integer coordinates is exact);
///  * DrawCachedBitmap needs a translate-only transform (else WrongState).

#include "winapi/graphics/gdiplus/gdiplus_image_internal.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_geometry.hpp"
#include "winapi/graphics/gdiplus/gdiplus_graphics.hpp"
#include "winapi/graphics/gdiplus/gdiplus_render.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vitreo::winapi::gdip {

namespace {

MetafileDrawer g_metafile_drawer = nullptr;
EffectApplier g_effect_applier = nullptr;

void finish(Marshaler& m, Status s) { (void)write_status(m, s); }

constexpr u64 kMaxRegionPixels = 1ull << 26;

/// Device pixels per page unit at @p dpi, times the page scale: the factor
/// that turns a physical image size (in device pixels) into world units.
float page_factor(const GpGraphics& g, float dpi) {
    return unit_to_device(g.state.page_unit, dpi) * g.state.page_scale;
}

/// Source unit -> image pixels; false for UnitWorld / UnitDisplay or an
/// unknown unit (probe "pointsrect_world=2 pointsrect_display=2").
bool src_to_pixels(i32 unit, const GpImage& img, RectF& r) {
    float sx = 1, sy = 1;
    switch (unit) {
    case UnitPixel: break;
    case UnitPoint: sx = img.dpi_x / 72.0f; sy = img.dpi_y / 72.0f; break;
    case UnitInch: sx = img.dpi_x; sy = img.dpi_y; break;
    case UnitDocument: sx = img.dpi_x / 300.0f; sy = img.dpi_y / 300.0f; break;
    case UnitMillimeter: sx = img.dpi_x / 25.4f; sy = img.dpi_y / 25.4f; break;
    default: return false;
    }
    r.X *= sx;
    r.Width *= sx;
    r.Y *= sy;
    r.Height *= sy;
    return true;
}

/// World size of @p px_w x @p px_h image pixels drawn at physical size.
PointF physical_world_size(const GpGraphics& g, const GpImage& img, float px_w, float px_h) {
    const float fx = page_factor(g, g.dpi_x), fy = page_factor(g, g.dpi_y);
    const float w = px_w * (g.dpi_x / (img.dpi_x > 0 ? img.dpi_x : 96.0f)) / (fx != 0 ? fx : 1.0f);
    const float h = px_h * (g.dpi_y / (img.dpi_y > 0 ? img.dpi_y : 96.0f)) / (fy != 0 ? fy : 1.0f);
    return {w, h};
}

/// Ask the abort callback (DrawImageAbort, BOOL CALLBACK(void*)) and the
/// image's GdiplusAbort whether to stop: Ok to go on, else the status the
/// draw answers.
Status should_abort(GdiplusManager& mgr, Marshaler& m, const GpImage& img, GuestAddr callback, GuestAddr data) {
    GdiManager* gdi = mgr.gdi();
    const auto call = gdi != nullptr ? gdi->guest_call() : GdiManager::GuestCall{};
    if (callback != 0) {
        if (!call) {
            log_unimplemented("GdipDrawImage*", "DrawImageAbort callback (no guest caller wired)");
        } else {
            const u64 args[1] = {data};
            if (static_cast<u32>(call(callback, args, 1, m.cpu_is_64bit())) != 0) return Status::Aborted;
        }
    }
    if (img.abort_object != 0) {
        // GdiplusAbort::Abort is vtable slot 0 (HRESULT __stdcall Abort()).
        // Any success code goes on; E_ABORT is Aborted, another failure
        // GenericError, nothing drawn (probe "imageabort ret=...").
        const auto vtbl = m.read_ptr(img.abort_object);
        const auto fn = vtbl.is_ok() ? m.read_ptr(vtbl.value()) : vtbl;
        if (!call || fn.is_err() || fn.value() == 0) {
            log_unimplemented("GdipDrawImage*", "GdiplusAbort object (no guest caller wired)");
        } else {
            const u64 args[1] = {img.abort_object};
            const u32 hr = static_cast<u32>(call(fn.value(), args, 1, m.cpu_is_64bit()));
            if (hr == 0x80004004u) return Status::Aborted;
            if (static_cast<i32>(hr) < 0) return Status::GenericError;
        }
    }
    return Status::Ok;
}

i32 wrap_coord(i32 v, i32 size, bool flip) {
    if (size <= 0) return 0;
    i32 q = v / size, r = v % size;
    if (r < 0) {
        r += size;
        --q;
    }
    if (flip && (q & 1) != 0) r = size - 1 - r;
    return r;
}

CGAffineTransform to_cg(const Affine& a) { return CGAffineTransformMake(a.m11, a.m12, a.m21, a.m22, a.dx, a.dy); }

bool half_pixel_offset(const GpGraphics& g) {
    return g.state.pixel_offset_mode == 2 || g.state.pixel_offset_mode == 4;  // Half, HighQuality
}

/// Where image pixels land. The scope's device space centres pixel i on
/// coordinate i (gdiplus_render.cpp); PixelOffsetModeHalf / HighQuality
/// move the world by -0.5, so an image rectangle covers whole pixels and
/// draws exactly, scaled or not (probe "pom2"/"pom4"). In the other modes
/// GDI+ blits an unscaled image exactly (probe "pom0 1:1") but maps a
/// scaled one with its edge on pixel centres, half a pixel up-left of the
/// Half-mode result (probe "pom0 ... 2x": ff0000 00ff00 00ff00 000000).
void align_image_pixels(CGContextRef cg, const GpGraphics& g, bool scaled) {
    if (half_pixel_offset(g)) return;
    const Affine w = world_to_device(g);
    Affine inv;
    if (!affine_invert(w, inv)) return;
    Affine shift;
    shift.dx = scaled ? -1.0f : -0.5f;
    shift.dy = scaled ? -1.0f : -0.5f;
    CGContextConcatCTM(cg, to_cg(w.then(shift).then(inv)));
}

/// Draw premultiplied BGRA @p px (rw x rh, top-down) so that the
/// region-local rectangle @p clip maps through @p a (region pixel -> world).
void draw_pixels(RenderScope& scope, const GpGraphics& g, const std::vector<u8>& px, i32 rw, i32 rh,
                 const Affine& a, const RectF& clip) {
    const Affine dev = a.then(world_to_device(g));
    auto near = [](float v, float want) { return std::fabs(v - want) < 1e-5f; };
    const bool scaled = !(near(dev.m11, 1) && near(dev.m22, 1) && near(dev.m12, 0) && near(dev.m21, 0));
    CGContextRef cg = scope.cg();
    if (cg == nullptr || rw <= 0 || rh <= 0) return;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, px.data(), px.size(), nullptr);
    CGImageRef image = CGImageCreate(static_cast<size_t>(rw), static_cast<size_t>(rh), 8, 32,
                                     static_cast<size_t>(rw) * 4u, space,
                                     static_cast<CGBitmapInfo>(static_cast<u32>(kCGImageAlphaPremultipliedFirst) |
                                                               static_cast<u32>(kCGBitmapByteOrder32Little)),
                                     dp, nullptr, false, kCGRenderingIntentDefault);
    CGContextSaveGState(cg);
    align_image_pixels(cg, g, scaled);
    CGContextConcatCTM(cg, to_cg(a));
    // An aliased draw covers the device pixels whose centres lie inside
    // the source rectangle's image, left/top edges in, right/bottom out
    // (GDI+'s rule; probe "pom0 ... 2x": the column past a half-pixel-
    // shifted image stays clear). For an axis-aligned draw the clip is
    // snapped to exactly those pixels; otherwise the path clip stands.
    CGRect r = CGRectStandardize(CGRectMake(clip.X, clip.Y, clip.Width, clip.Height));
    const bool aa = g.state.smoothing_mode == 2 || g.state.smoothing_mode == 4 || g.state.smoothing_mode == 5;
    const CGAffineTransform ctm = CGContextGetCTM(cg);
    if (!aa && std::fabs(ctm.b) < 1e-6 && std::fabs(ctm.c) < 1e-6) {
        const CGRect d = CGRectStandardize(CGContextConvertRectToDeviceSpace(cg, r));
        const CGFloat x0 = std::ceil(d.origin.x - 0.5), x1 = std::ceil(d.origin.x + d.size.width - 0.5);
        const CGFloat y0 = std::ceil(d.origin.y - 0.5), y1 = std::ceil(d.origin.y + d.size.height - 0.5);
        r = CGContextConvertRectToUserSpace(cg, CGRectMake(x0, y0, std::max<CGFloat>(0, x1 - x0),
                                                           std::max<CGFloat>(0, y1 - y0)));
    }
    CGContextClipToRect(cg, CGRectStandardize(r));
    CGContextTranslateCTM(cg, 0, rh);
    CGContextScaleCTM(cg, 1, -1);
    if (image != nullptr) CGContextDrawImage(cg, CGRectMake(0, 0, rw, rh), image);
    CGContextRestoreGState(cg);
    if (image != nullptr) CGImageRelease(image);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(space);
}

/// The DrawImage core. @p dst: world-space top-left, top-right,
/// bottom-left; @p src in @p src_unit.
Status draw_image(GdiplusManager& mgr, Marshaler& m, u64 gh, u64 ih, const PointF dst[3], RectF src, i32 src_unit,
                  u64 ia_h, GuestAddr callback, GuestAddr cb_data, u64 effect) {
    GpGraphics* g = mgr.objects().get<GpGraphics>(gh);
    GpImage* img = mgr.objects().get<GpImage>(ih);
    // A wrong-class image or attributes is ObjectBusy; a wrong-class
    // graphics InvalidParameter (probe "draw_img=4 draw_g=2 draw_ia=4").
    if (g == nullptr) return Status::InvalidParameter;
    if (img == nullptr) return missing_object(mgr, ih);
    if (const Status s = graphics_usable(g); s != Status::Ok) return s;
    const GpImageAttributes* ia = nullptr;
    if (ia_h != 0) {
        ia = mgr.objects().get<GpImageAttributes>(ia_h);
        if (ia == nullptr) return missing_object(mgr, ia_h);
    }
    if (img->locked != 0) return Status::WrongState;  // probe "draw_locked=8"
    if (img->type != ImageTypeBitmap) {
        if (const Status a = should_abort(mgr, m, *img, callback, cb_data); a != Status::Ok) return a;
        if (g_metafile_drawer == nullptr) {
            log_unimplemented("GdipDrawImage*", "drawing a metafile (no metafile player installed)");
            return Status::NotImplemented;
        }
        return g_metafile_drawer(mgr, m, *g, *img, dst, src, ia);
    }
    if (!src_to_pixels(src_unit, *img, src)) return Status::InvalidParameter;
    if (effect != 0 && g_effect_applier == nullptr) {
        log_unimplemented("GdipDrawImageFX", "applying a CGpEffect (no effect implementation installed)");
        return Status::NotImplemented;
    }
    if (const Status a = should_abort(mgr, m, *img, callback, cb_data); a != Status::Ok) return a;
    if (src.Width == 0 || src.Height == 0 || !std::isfinite(src.Width) || !std::isfinite(src.Height)) return Status::Ok;

    // Integer region of source pixels the rectangle touches.
    const float sx0 = std::min(src.X, src.X + src.Width), sx1 = std::max(src.X, src.X + src.Width);
    const float sy0 = std::min(src.Y, src.Y + src.Height), sy1 = std::max(src.Y, src.Y + src.Height);
    i32 rx0 = static_cast<i32>(std::floor(sx0)), rx1 = static_cast<i32>(std::ceil(sx1));
    i32 ry0 = static_cast<i32>(std::floor(sy0)), ry1 = static_cast<i32>(std::ceil(sy1));
    const i32 wrap = ia != nullptr ? ia->wrap_mode : 4;
    const u32 border = ia != nullptr ? ia->wrap_color : 0u;
    const bool tiled = wrap >= 0 && wrap <= 3;
    if (!tiled && (border >> 24) == 0) {
        // Clamp to transparent: nothing outside the image draws.
        rx0 = std::max(rx0, 0);
        ry0 = std::max(ry0, 0);
        rx1 = std::min(rx1, img->width);
        ry1 = std::min(ry1, img->height);
        if (rx1 <= rx0 || ry1 <= ry0) return Status::Ok;
    }
    const i32 rw = rx1 - rx0, rh = ry1 - ry0;
    if (rw <= 0 || rh <= 0) return Status::Ok;
    if (static_cast<u64>(rw) * static_cast<u64>(rh) > kMaxRegionPixels) return Status::OutOfMemory;

    // Source pixels: the part inside the image, adjusted; the rest from
    // the wrap mode.
    std::vector<u32> region(static_cast<usize>(rw) * static_cast<usize>(rh), border);
    const i32 ix0 = tiled ? 0 : std::max(rx0, 0), iy0 = tiled ? 0 : std::max(ry0, 0);
    const i32 ix1 = tiled ? img->width : std::min(rx1, img->width), iy1 = tiled ? img->height : std::min(ry1, img->height);
    std::vector<u32> inside;
    if (ix1 > ix0 && iy1 > iy0) {
        if (!image_read_argb(m, *img, ix0, iy0, ix1 - ix0, iy1 - iy0, inside)) {
            return Status::InvalidParameter;  // probe "draw_gray=2 draw_cmyk=2"
        }
        if (ia != nullptr) apply_color_adjust(effective_adjust(*ia, ColorAdjustTypeBitmap), inside.data(), inside.size());
    }
    const i32 iw = ix1 - ix0;
    for (i32 v = 0; v < rh; ++v) {
        for (i32 u = 0; u < rw; ++u) {
            i32 x = rx0 + u, y = ry0 + v;
            if (tiled) {
                x = wrap_coord(x, img->width, wrap == 1 || wrap == 3);
                y = wrap_coord(y, img->height, wrap == 2 || wrap == 3);
            } else if (x < 0 || y < 0 || x >= img->width || y >= img->height) {
                continue;
            }
            region[static_cast<usize>(v) * static_cast<usize>(rw) + static_cast<usize>(u)] =
                inside[static_cast<usize>(y - iy0) * static_cast<usize>(iw) + static_cast<usize>(x - ix0)];
        }
    }
    if (effect != 0) {
        const Status s = g_effect_applier(mgr, effect, region, rw, rh);
        if (s != Status::Ok) return s;
    }
    std::vector<u8> px(region.size() * 4);
    argb_to_render(region.data(), region.size(), px.data());

    // Region pixel (u, v) -> world: source point (rx0 + u, ry0 + v) mapped
    // through the parallelogram.
    Affine a;
    a.m11 = (dst[1].X - dst[0].X) / src.Width;
    a.m12 = (dst[1].Y - dst[0].Y) / src.Width;
    a.m21 = (dst[2].X - dst[0].X) / src.Height;
    a.m22 = (dst[2].Y - dst[0].Y) / src.Height;
    a.dx = dst[0].X + (static_cast<float>(rx0) - src.X) * a.m11 + (static_cast<float>(ry0) - src.Y) * a.m21;
    a.dy = dst[0].Y + (static_cast<float>(rx0) - src.X) * a.m12 + (static_cast<float>(ry0) - src.Y) * a.m22;
    const RectF clip{src.X - static_cast<float>(rx0), src.Y - static_cast<float>(ry0), src.Width, src.Height};

    RenderScope scope(mgr, m, *g);
    if (scope.status() != Status::Ok) return scope.status();
    draw_pixels(scope, *g, px, rw, rh, a, clip);
    return scope.commit();
}

std::array<PointF, 3> rect_points(float x, float y, float w, float h) {
    return {PointF{x, y}, PointF{x + w, y}, PointF{x, y + h}};
}

RectF image_px_rect(const GpImage& img) {
    if (img.type != ImageTypeBitmap) return img.bounds;
    return {0, 0, static_cast<float>(img.width), static_cast<float>(img.height)};
}

/// The physical-size destination of a whole image at (x, y).
std::array<PointF, 3> physical_points(const GpGraphics& g, const GpImage& img, float x, float y, float px_w,
                                      float px_h) {
    const PointF s = physical_world_size(g, img, px_w, px_h);
    return rect_points(x, y, s.X, s.Y);
}

Status draw_at(GdiplusManager& mgr, Marshaler& m, u64 gh, u64 ih, float x, float y) {
    const GpGraphics* g = mgr.objects().get<GpGraphics>(gh);
    const GpImage* img = mgr.objects().get<GpImage>(ih);
    if (g == nullptr) return Status::InvalidParameter;
    if (img == nullptr) return missing_object(mgr, ih);
    const RectF src = image_px_rect(*img);
    const auto pts = physical_points(*g, *img, x, y, src.Width, src.Height);
    return draw_image(mgr, m, gh, ih, pts.data(), src, img->type == ImageTypeBitmap ? UnitPixel : img->bounds_unit, 0, 0, 0, 0);
}

Status draw_rect(GdiplusManager& mgr, Marshaler& m, u64 gh, u64 ih, float x, float y, float w, float h) {
    const GpImage* img = mgr.objects().get<GpImage>(ih);
    if (img == nullptr) return missing_object(mgr, ih);
    const auto pts = rect_points(x, y, w, h);
    return draw_image(mgr, m, gh, ih, pts.data(), image_px_rect(*img),
                      img->type == ImageTypeBitmap ? UnitPixel : img->bounds_unit, 0, 0, 0, 0);
}

Status draw_point_rect(GdiplusManager& mgr, Marshaler& m, u64 gh, u64 ih, float x, float y, RectF src, i32 unit) {
    const GpGraphics* g = mgr.objects().get<GpGraphics>(gh);
    const GpImage* img = mgr.objects().get<GpImage>(ih);
    if (g == nullptr) return Status::InvalidParameter;
    if (img == nullptr) return missing_object(mgr, ih);
    RectF px = src;
    if (img->type == ImageTypeBitmap && !src_to_pixels(unit, *img, px)) return Status::InvalidParameter;
    const auto pts = physical_points(*g, *img, x, y, px.Width, px.Height);
    return draw_image(mgr, m, gh, ih, pts.data(), src, unit, 0, 0, 0, 0);
}

Status points_status(i32 count, GuestAddr pts) {
    if (pts == 0) return Status::InvalidParameter;
    if (count == 4) {
        log_unimplemented("GdipDrawImagePoints*", "4 destination points (GDI+ refuses it too)");
        return Status::NotImplemented;  // probe "points4=6"
    }
    return count == 3 ? Status::Ok : Status::InvalidParameter;
}

}  // namespace

void set_metafile_drawer(MetafileDrawer drawer) { g_metafile_drawer = drawer; }
void set_effect_applier(EffectApplier applier) { g_effect_applier = applier; }

void register_image_draw(GdiplusManager& mgr, HandlerMap& map) {
    // GdipGetImageGraphicsContext(GpImage*, GpGraphics**): indexed formats
    // are accepted (probe "graphics=0" for 1/4/8bpp, contrary to the C++
    // wrapper's note); 16bppARGB1555, 16bppGrayScale and CMYK are
    // OutOfMemory (probe "graphics=3").
    map["GdipGetImageGraphicsContext"] = [&mgr](Marshaler& m) {
        const u64 ih = handle_arg(m, 0);
        const GpImage* img = mgr.objects().get<GpImage>(ih);
        if (img == nullptr) return finish(m, missing_object(mgr, ih));
        const GuestAddr out = m.arg_ptr(1);
        if (img == nullptr || out == 0) return finish(m, Status::InvalidParameter);
        if (img->type == ImageTypeBitmap) {
            const i32 f = pixel_format_canonical(img->pixel_format);
            if (f == PixelFormat16bppARGB1555 || pixel_format_opaque_storage(f)) return finish(m, Status::OutOfMemory);
        }
        GpGraphics g;
        g.kind = TargetKind::Image;
        g.image = ih;
        g.dpi_x = img->dpi_x;
        g.dpi_y = img->dpi_y;
        const u64 h = mgr.objects().add(std::move(g));
        if (!write_handle(m, out, h)) {
            (void)mgr.objects().remove<GpGraphics>(h);
            return finish(m, Status::InvalidParameter);
        }
        finish(m, Status::Ok);
    };

    // GdipDrawImage(GpGraphics*, GpImage*, REAL x, REAL y)
    map["GdipDrawImage"] = [&mgr](Marshaler& m) {
        finish(m, draw_at(mgr, m, handle_arg(m, 0), handle_arg(m, 1), real_arg(m, 2), real_arg(m, 3)));
    };
    map["GdipDrawImageI"] = [&mgr](Marshaler& m) {
        finish(m, draw_at(mgr, m, handle_arg(m, 0), handle_arg(m, 1), static_cast<float>(static_cast<i32>(m.arg32(2))),
                          static_cast<float>(static_cast<i32>(m.arg32(3)))));
    };
    // GdipDrawImageRect(GpGraphics*, GpImage*, REAL x, REAL y, REAL w, REAL h)
    map["GdipDrawImageRect"] = [&mgr](Marshaler& m) {
        finish(m, draw_rect(mgr, m, handle_arg(m, 0), handle_arg(m, 1), real_arg(m, 2), real_arg(m, 3), real_arg(m, 4),
                            real_arg(m, 5)));
    };
    map["GdipDrawImageRectI"] = [&mgr](Marshaler& m) {
        auto i = [&m](usize k) { return static_cast<float>(static_cast<i32>(m.arg32(k))); };
        finish(m, draw_rect(mgr, m, handle_arg(m, 0), handle_arg(m, 1), i(2), i(3), i(4), i(5)));
    };
    // GdipDrawImagePointRect(g, img, REAL x, REAL y, REAL srcx, REAL srcy, REAL srcw, REAL srch, GpUnit)
    map["GdipDrawImagePointRect"] = [&mgr](Marshaler& m) {
        finish(m, draw_point_rect(mgr, m, handle_arg(m, 0), handle_arg(m, 1), real_arg(m, 2), real_arg(m, 3),
                                  RectF{real_arg(m, 4), real_arg(m, 5), real_arg(m, 6), real_arg(m, 7)},
                                  static_cast<i32>(m.arg32(8))));
    };
    map["GdipDrawImagePointRectI"] = [&mgr](Marshaler& m) {
        auto i = [&m](usize k) { return static_cast<float>(static_cast<i32>(m.arg32(k))); };
        finish(m, draw_point_rect(mgr, m, handle_arg(m, 0), handle_arg(m, 1), i(2), i(3), RectF{i(4), i(5), i(6), i(7)},
                                  static_cast<i32>(m.arg32(8))));
    };
    // GdipDrawImagePoints(g, img, const GpPointF*, INT count)
    auto draw_points = [&mgr](Marshaler& m, bool integer) {
        const u64 gh = handle_arg(m, 0), ih = handle_arg(m, 1);
        const GuestAddr at = m.arg_ptr(2);
        const i32 count = static_cast<i32>(m.arg32(3));
        const GpImage* img = mgr.objects().get<GpImage>(ih);
        if (mgr.objects().get<GpGraphics>(gh) == nullptr) return finish(m, Status::InvalidParameter);
        if (img == nullptr) return finish(m, missing_object(mgr, ih));
        if (const Status s = points_status(count, at); s != Status::Ok) return finish(m, s);
        std::vector<PointF> pts;
        if (!(integer ? read_points_i(m, at, 3, pts) : read_points(m, at, 3, pts))) return finish(m, Status::InvalidParameter);
        finish(m, draw_image(mgr, m, gh, ih, pts.data(), image_px_rect(*img),
                             img->type == ImageTypeBitmap ? UnitPixel : img->bounds_unit, 0, 0, 0, 0));
    };
    map["GdipDrawImagePoints"] = [draw_points](Marshaler& m) { draw_points(m, false); };
    map["GdipDrawImagePointsI"] = [draw_points](Marshaler& m) { draw_points(m, true); };
    // GdipDrawImagePointsRect(g, img, const GpPointF*, INT count, REAL srcx, REAL srcy,
    //   REAL srcw, REAL srch, GpUnit, const GpImageAttributes*, DrawImageAbort, VOID*)
    auto draw_points_rect = [&mgr](Marshaler& m, bool integer) {
        const u64 gh = handle_arg(m, 0), ih = handle_arg(m, 1);
        const GuestAddr at = m.arg_ptr(2);
        const i32 count = static_cast<i32>(m.arg32(3));
        auto r = [&m, integer](usize k) {
            return integer ? static_cast<float>(static_cast<i32>(m.arg32(k))) : real_arg(m, k);
        };
        const RectF src{r(4), r(5), r(6), r(7)};
        if (mgr.objects().get<GpGraphics>(gh) == nullptr) return finish(m, Status::InvalidParameter);
        if (mgr.objects().get<GpImage>(ih) == nullptr) return finish(m, missing_object(mgr, ih));
        if (const Status s = points_status(count, at); s != Status::Ok) return finish(m, s);
        std::vector<PointF> pts;
        if (!(integer ? read_points_i(m, at, 3, pts) : read_points(m, at, 3, pts))) return finish(m, Status::InvalidParameter);
        finish(m, draw_image(mgr, m, gh, ih, pts.data(), src, static_cast<i32>(m.arg32(8)), handle_arg(m, 9),
                             m.arg_ptr(10), m.arg_ptr(11), 0));
    };
    map["GdipDrawImagePointsRect"] = [draw_points_rect](Marshaler& m) { draw_points_rect(m, false); };
    map["GdipDrawImagePointsRectI"] = [draw_points_rect](Marshaler& m) { draw_points_rect(m, true); };
    // GdipDrawImageRectRect(g, img, REAL dstx, dsty, dstw, dsth, REAL srcx, srcy, srcw, srch,
    //   GpUnit, const GpImageAttributes*, DrawImageAbort, VOID*)
    auto draw_rect_rect = [&mgr](Marshaler& m, bool integer) {
        auto r = [&m, integer](usize k) {
            return integer ? static_cast<float>(static_cast<i32>(m.arg32(k))) : real_arg(m, k);
        };
        const auto pts = rect_points(r(2), r(3), r(4), r(5));
        const RectF src{r(6), r(7), r(8), r(9)};
        finish(m, draw_image(mgr, m, handle_arg(m, 0), handle_arg(m, 1), pts.data(), src, static_cast<i32>(m.arg32(10)),
                             handle_arg(m, 11), m.arg_ptr(12), m.arg_ptr(13), 0));
    };
    map["GdipDrawImageRectRect"] = [draw_rect_rect](Marshaler& m) { draw_rect_rect(m, false); };
    map["GdipDrawImageRectRectI"] = [draw_rect_rect](Marshaler& m) { draw_rect_rect(m, true); };
    // GdipDrawImageFX(g, img, GpRectF* source, GpMatrix* xForm, CGpEffect*, GpImageAttributes*, GpUnit):
    // the source rectangle (default: the image bounds) drawn at itself
    // through xForm (probe "fx_matrix ... minx=1").
    map["GdipDrawImageFX"] = [&mgr](Marshaler& m) {
        const u64 gh = handle_arg(m, 0), ih = handle_arg(m, 1);
        const GuestAddr src_at = m.arg_ptr(2);
        const u64 xform = handle_arg(m, 3), effect = handle_arg(m, 4), ia = handle_arg(m, 5);
        const i32 unit = static_cast<i32>(m.arg32(6));
        const GpImage* img = mgr.objects().get<GpImage>(ih);
        if (mgr.objects().get<GpGraphics>(gh) == nullptr) return finish(m, Status::InvalidParameter);
        if (img == nullptr) return finish(m, missing_object(mgr, ih));
        RectF src = image_px_rect(*img);
        i32 src_unit = img->type == ImageTypeBitmap ? UnitPixel : img->bounds_unit;
        if (src_at != 0) {
            if (!read_rect(m, src_at, src)) return finish(m, Status::InvalidParameter);
            src_unit = unit;
        }
        Affine x;
        if (xform != 0) {
            const GpMatrix* mat = mgr.objects().get<GpMatrix>(xform);
            if (mat == nullptr) return finish(m, Status::InvalidParameter);
            x = mat->m;
        }
        RectF dst_px = src;
        if (img->type == ImageTypeBitmap && !src_to_pixels(src_unit, *img, dst_px)) return finish(m, Status::InvalidParameter);
        const PointF pts[3] = {x.apply({dst_px.X, dst_px.Y}), x.apply({dst_px.X + dst_px.Width, dst_px.Y}),
                               x.apply({dst_px.X, dst_px.Y + dst_px.Height})};
        finish(m, draw_image(mgr, m, gh, ih, pts, src, src_unit, ia, 0, 0, effect));
    };

    // ---- Cached bitmaps ------------------------------------------------------------
    map["GdipCreateCachedBitmap"] = [&mgr](Marshaler& m) {
        const GpImage* img = mgr.objects().get<GpImage>(handle_arg(m, 0));
        const GpGraphics* g = mgr.objects().get<GpGraphics>(handle_arg(m, 1));
        const GuestAddr out = m.arg_ptr(2);
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        if (img == nullptr || g == nullptr || out == 0 || img->type != ImageTypeBitmap)
            return finish(m, Status::InvalidParameter);
        GpCachedBitmap cb;
        if (!image_to_render(m, *img, cb.pixels)) return finish(m, Status::InvalidParameter);
        const u64 h = mgr.objects().add(std::move(cb));
        if (!write_handle(m, out, h)) {
            (void)mgr.objects().remove<GpCachedBitmap>(h);
            return finish(m, Status::InvalidParameter);
        }
        finish(m, Status::Ok);
    };
    map["GdipDeleteCachedBitmap"] = [&mgr](Marshaler& m) {
        finish(m, mgr.objects().remove<GpCachedBitmap>(handle_arg(m, 0)) ? Status::Ok : Status::InvalidParameter);
    };
    // GdipDrawCachedBitmap(GpGraphics*, GpCachedBitmap*, INT x, INT y): 1:1
    // at the translated device position; any rotation or scale is
    // WrongState (probe "draw_rotated=8 draw_scaled=8").
    map["GdipDrawCachedBitmap"] = [&mgr](Marshaler& m) {
        GpGraphics* g = mgr.objects().get<GpGraphics>(handle_arg(m, 0));
        const GpCachedBitmap* cb = mgr.objects().get<GpCachedBitmap>(handle_arg(m, 1));
        const i32 x = static_cast<i32>(m.arg32(2)), y = static_cast<i32>(m.arg32(3));
        if (g == nullptr || cb == nullptr) return finish(m, Status::InvalidParameter);
        if (const Status s = graphics_usable(g); s != Status::Ok) return finish(m, s);
        const Affine w = world_to_device(*g);
        if (w.m11 != 1.0f || w.m22 != 1.0f || w.m12 != 0.0f || w.m21 != 0.0f) return finish(m, Status::WrongState);
        RenderScope scope(mgr, m, *g);
        if (scope.status() != Status::Ok) return finish(m, scope.status());
        Affine a;
        a.dx = static_cast<float>(x);
        a.dy = static_cast<float>(y);
        draw_pixels(scope, *g, cb->pixels.bgra, cb->pixels.width, cb->pixels.height, a,
                    RectF{0, 0, static_cast<float>(cb->pixels.width), static_cast<float>(cb->pixels.height)});
        finish(m, scope.commit());
    };
}

}  // namespace vitreo::winapi::gdip
