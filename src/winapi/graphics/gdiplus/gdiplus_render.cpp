// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_render.cpp
/// @brief RenderScope: GDI+ drawing through CoreGraphics (see gdiplus_render.hpp).

#include "winapi/graphics/gdiplus/gdiplus_render.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_image.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::gdip {

// unit_to_device / world_to_device: gdiplus_graphics.cpp (graphics area).

RenderScope::RenderScope(GdiplusManager& mgr, Marshaler& m, GpGraphics& g)
    : m_mgr(mgr), m_m(m), m_g(g) {
    if (g.hdc_out) {  // between GdipGetDC and GdipReleaseDC
        m_status = Status::ObjectBusy;
        return;
    }
    if (g.kind == TargetKind::Image) {
        GpImage* image = mgr.objects().get<GpImage>(g.image);
        if (image == nullptr) { m_status = Status::InvalidParameter; return; }
        if (image->type != ImageTypeBitmap) {
            log_unimplemented("RenderScope", "drawing into a metafile (EMF+ recording)");
            m_status = Status::NotImplemented;
            return;
        }
        RenderPixels px;
        if (!image_to_render(m, *image, px)) { m_status = Status::NotImplemented; return; }
        m_width = px.width;
        m_height = px.height;
        m_canvas = std::move(px.bgra);
    } else {
        GdiManager* gdi = mgr.gdi();
        if (gdi == nullptr) { m_status = Status::GenericError; return; }
        bool copied = false;
        gdi->with_device_surface(static_cast<Handle>(g.hdc), [&](GdiManager::DeviceSurface& s) {
            m_width = s.width;
            m_height = s.height;
            m_canvas.resize(static_cast<usize>(s.width) * static_cast<usize>(s.height) * 4u);
            // GDI+ draws on a DC's bitmap as on 32bpp PARGB: the stored
            // bytes are taken as premultiplied, unclamped, alpha included
            // (gdiplus_graphics_probe "alpha pre=...", "ddb", "dib32 ...
            // over 10ffffff"). A window surface stays opaque.
            const bool window = s.alpha == 0xFF;
            for (usize i = 0; i < m_canvas.size(); i += 4) {
                m_canvas[i] = s.rgba[i + 2];
                m_canvas[i + 1] = s.rgba[i + 1];
                m_canvas[i + 2] = s.rgba[i];
                m_canvas[i + 3] = window ? 0xFF : s.rgba[i + 3];
            }
            copied = true;
        });
        if (!copied) { m_status = Status::InvalidParameter; return; }
        m_snapshot = m_canvas;
    }
    if (m_width <= 0 || m_height <= 0) { m_status = Status::Ok; return; }

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    m_cg = CGBitmapContextCreate(m_canvas.data(), static_cast<size_t>(m_width),
                                 static_cast<size_t>(m_height), 8,
                                 static_cast<size_t>(m_width) * 4u, space,
                                 static_cast<u32>(kCGImageAlphaPremultipliedFirst) |
                                     static_cast<u32>(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(space);
    if (m_cg == nullptr) { m_status = Status::OutOfMemory; return; }

    // Device space: y down, origin top-left, as GDI+.
    CGContextTranslateCTM(m_cg, 0, m_height);
    CGContextScaleCTM(m_cg, 1, -1);
    // An HDC target draws in the DC's device pixels: world_to_device ends
    // in the DC's logical->device transform captured at creation
    // (GraphicsState::container_world; probe "vporg", "winorg", "aniso").
    clip_to_region(m_cg, g.state.clip.get());
    clip_to_region(m_cg, g.state.container_clip.get());
    // Pixel centres (winoracle gdiplus_draw_probe, fillrect-* rows): device
    // pixel i is centred on device coordinate i (world_to_device carries the
    // PixelOffsetMode half-pixel shift). A canvas pixel i spans [i, i+1], so
    // device space draws shifted by +0.5.
    CGContextTranslateCTM(m_cg, 0.5, 0.5);
    const Affine w = world_to_device(g);
    CGContextConcatCTM(m_cg, CGAffineTransformMake(w.m11, w.m12, w.m21, w.m22, w.dx, w.dy));

    const i32 smoothing = g.state.smoothing_mode;
    const bool aa = smoothing == 2 || smoothing == 4 || smoothing == 5;  // HighQuality, AntiAlias*
    CGContextSetShouldAntialias(m_cg, aa);
    CGContextSetAllowsAntialiasing(m_cg, aa);
    switch (g.state.interpolation_mode) {
    case 5: CGContextSetInterpolationQuality(m_cg, kCGInterpolationNone); break;  // NearestNeighbor
    case 1: CGContextSetInterpolationQuality(m_cg, kCGInterpolationLow); break;   // LowQuality
    case 2: case 6: case 7:
        CGContextSetInterpolationQuality(m_cg, kCGInterpolationHigh); break;
    default: CGContextSetInterpolationQuality(m_cg, kCGInterpolationMedium); break;
    }
    if (g.state.compositing_mode == 1) CGContextSetBlendMode(m_cg, kCGBlendModeCopy);
}

RenderScope::~RenderScope() {
    (void)commit();
    if (m_cg != nullptr) CGContextRelease(m_cg);
}

Status RenderScope::commit() {
    if (m_committed || m_status != Status::Ok || m_width <= 0 || m_height <= 0) return m_status;
    m_committed = true;
    if (m_cg != nullptr) CGContextFlush(m_cg);
    if (m_g.kind == TargetKind::Image) {
        GpImage* image = m_mgr.objects().get<GpImage>(m_g.image);
        if (image == nullptr) return Status::InvalidParameter;
        RenderPixels px;
        px.width = m_width;
        px.height = m_height;
        px.bgra = m_canvas;
        return render_to_image(m_m, *image, px) ? Status::Ok : Status::NotImplemented;
    }
    GdiManager* gdi = m_mgr.gdi();
    if (gdi == nullptr) return Status::GenericError;
    const bool ok = gdi->with_device_surface(
        static_cast<Handle>(m_g.hdc), [&](GdiManager::DeviceSurface& s) {
            if (s.width != m_width || s.height != m_height) return;
            // Store only what this draw changed, inside the DC clip, so
            // GDI drawing on the same DC since the copy survives.
            for (i32 y = 0; y < m_height; ++y) {
                for (i32 x = 0; x < m_width; ++x) {
                    const usize i = (static_cast<usize>(y) * static_cast<usize>(m_width) +
                                     static_cast<usize>(x)) * 4u;
                    if (std::memcmp(&m_canvas[i], &m_snapshot[i], 4) == 0) continue;
                    if (s.clip != nullptr && !s.clip->contains(x, y)) continue;
                    // Stored as drawn, premultiplied with its alpha (32bpp
                    // PARGB semantics; see the copy-in above).
                    u8 r = m_canvas[i + 2], gch = m_canvas[i + 1], b = m_canvas[i];
                    if (s.mono) {
                        const bool white = (r + gch + b) >= 3 * 128;
                        r = gch = b = white ? 0xFF : 0x00;
                    }
                    s.rgba[i] = r;
                    s.rgba[i + 1] = gch;
                    s.rgba[i + 2] = b;
                    s.rgba[i + 3] = s.alpha == 0xFF ? 0xFF : m_canvas[i + 3];
                }
            }
        });
    return ok ? Status::Ok : Status::InvalidParameter;
}

}  // namespace vitreo::winapi::gdip
