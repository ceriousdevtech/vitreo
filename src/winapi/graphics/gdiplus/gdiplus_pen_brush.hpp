#pragma once
/// @file gdiplus_pen_brush.hpp
/// @brief GpBrush (every GDI+ BrushType) and the brush fill every Fill*
/// export, text and region painting renders through.
///
/// Contract sources: GdiPlusFlat.h, GdiPlusEnums.h (BrushType, HatchStyle,
/// WrapMode, LinearGradientMode), GdiPlusBrush.h / GdiPlusPath.h, the
/// Microsoft Learn Brush / SolidBrush / HatchBrush / TextureBrush /
/// LinearGradientBrush / PathGradientBrush pages, and the winoracle probe
/// tools/oracle_probe/gdiplus_brush_probe.c (.expected = x64,
/// .x86.expected = x86), which supplies every default, status code,
/// geometry rule and rendered colour Learn does not state. The contract
/// notes and the deliberate deviations are in .docs/contracts/gdiplus/pen_brush.md.

#include "winapi/graphics/gdiplus/gdiplus_core.hpp"
#include "winapi/graphics/gdiplus/gdiplus_image.hpp"

#include <memory>
#include <vector>

typedef const struct CGPath* CGPathRef;

namespace vitreo::winapi::gdip {

class RenderScope;

/// GDI+ `BrushType` (GdiPlusEnums.h).
enum BrushType : i32 {
    BrushTypeSolidColor = 0,
    BrushTypeHatchFill = 1,
    BrushTypeTextureFill = 2,
    BrushTypePathGradient = 3,
    BrushTypeLinearGradient = 4,
};

/// GDI+ `WrapMode` (GdiPlusEnums.h).
enum WrapMode : i32 {
    WrapModeTile = 0,
    WrapModeTileFlipX = 1,
    WrapModeTileFlipY = 2,
    WrapModeTileFlipXY = 3,
    WrapModeClamp = 4,
};

/// A blend curve (Brush::SetBlend): factor of the second colour at each
/// position. One entry is GDI+'s default; its factor acts as an exponent
/// (probe: "linefill blend1 .5").
struct GradientBlend {
    std::vector<float> factors{1.0f};
    std::vector<float> positions{0.0f};
};

/// Interpolation colours (SetInterpolationColors / PresetBlend). Empty when
/// not in use; while set they replace the two colours and the blend.
struct GradientPreset {
    std::vector<u32> colors;
    std::vector<float> positions;
};

struct GpBrush {
    i32 type{BrushTypeSolidColor};

    /// SolidColor colour; HatchFill foreground.
    u32 color{0};

    // HatchFill.
    i32 hatch_style{0};
    u32 hatch_back{0};

    /// Texture and both gradients; stored as given (GDI+ keeps invalid
    /// values passed at creation of a linear gradient).
    i32 wrap{WrapModeTile};
    /// Brush space -> world. For a linear gradient it includes the
    /// rect/angle mapping GDI+ derives at creation (GetLineTransform reports it).
    Affine transform;

    // TextureFill: the brush's own copy of the source pixels (source rect
    // and image attributes applied), premultiplied BGRA, plus the image
    // GetTextureImage clones (PixelFormat32bppPARGB, as GDI+ reports).
    std::shared_ptr<const GpImage> texture;

    /// LinearGradient: the gradient runs along brush-space x across
    /// [rect.X, rect.X + rect.Width]. PathGradient: bounds of the points.
    RectF rect;
    u32 line_colors[2]{0, 0};
    i32 gamma{0};  ///< BOOL as given (GetXxxGammaCorrection returns it unchanged)
    GradientBlend blend;
    GradientPreset preset;

    // PathGradient (brush space).
    std::vector<PointF> path_points;
    std::vector<u8> path_types;  ///< PathPointType bytes
    u32 center_color{0};
    PointF center;
    std::vector<u32> surround{0xFFFFFFFFu};  ///< as set; the last repeats
    PointF focus{0, 0};
};

/// Fill @p path (WORLD coordinates; the scope's CTM maps world -> device)
/// with @p brush. Handles every brush type, wrap mode and brush transform.
Status fill_path(RenderScope& scope, CGPathRef path, bool even_odd, const GpBrush& brush);

/// True (with the colour) for a SolidColor brush.
[[nodiscard]] bool brush_solid_argb(const GpBrush& brush, u32& argb);

}  // namespace vitreo::winapi::gdip
