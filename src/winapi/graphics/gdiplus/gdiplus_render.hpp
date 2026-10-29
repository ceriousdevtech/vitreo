#pragma once
/// @file gdiplus_render.hpp
/// @brief The GDI+ rasterizer: a CoreGraphics context over a GpGraphics'
/// target with that graphics' transform, clip and quality state applied.
///
/// Every Draw* / Fill* / DrawString / DrawImage export renders through a
/// RenderScope. The scope copies the target's pixels into a premultiplied
/// BGRA canvas (an image's pixels, or the HDC's surface via
/// GdiManager::with_device_surface), hands out a CGContext whose CTM maps
/// WORLD coordinates to device pixels, and on commit() writes the canvas
/// back: into the image in its own pixel format, or into the DC surface
/// inside the DC's clip.

#include "winapi/graphics/gdiplus/gdiplus_graphics.hpp"

typedef struct CGContext* CGContextRef;
typedef struct CGPath* CGMutablePathRef;
typedef const struct CGPath* CGPathRef;

namespace vitreo::winapi {
class GdiManager;
}

namespace vitreo::winapi::gdip {

class RenderScope {
public:
    /// Open @p g's target. status() is not Ok when it cannot be drawn
    /// (unknown image, metafile target, DC without a surface, GetDC out).
    RenderScope(GdiplusManager& mgr, Marshaler& m, GpGraphics& g);
    ~RenderScope();
    RenderScope(const RenderScope&) = delete;
    RenderScope& operator=(const RenderScope&) = delete;

    [[nodiscard]] Status status() const { return m_status; }
    /// CTM = world -> device; clip = the graphics clip intersected with the
    /// target bounds (and the DC clip); antialiasing from SmoothingMode;
    /// interpolation from InterpolationMode; SourceCopy compositing as
    /// kCGBlendModeCopy.
    [[nodiscard]] CGContextRef cg() const { return m_cg; }
    /// Device-pixel size of the canvas.
    [[nodiscard]] i32 width() const { return m_width; }
    [[nodiscard]] i32 height() const { return m_height; }
    [[nodiscard]] const GpGraphics& graphics() const { return m_g; }
    /// Write the canvas back to the target. Idempotent; the destructor
    /// calls it.
    Status commit();

private:
    GdiplusManager& m_mgr;
    Marshaler& m_m;
    GpGraphics& m_g;
    Status m_status{Status::Ok};
    CGContextRef m_cg{nullptr};
    std::vector<u8> m_canvas;    ///< premultiplied BGRA
    std::vector<u8> m_snapshot;  ///< HDC target: the canvas as copied in
    i32 m_width{0};
    i32 m_height{0};
    bool m_committed{false};
};

/// Apply @p region (in device space) as the clip of @p cg. Implemented by
/// the region area (gdiplus_region.cpp); null = infinite.
void clip_to_region(CGContextRef cg, const GpRegion* region);

} // namespace vitreo::winapi::gdip
