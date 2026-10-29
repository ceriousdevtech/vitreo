#pragma once
/// @file gdiplus_pen.hpp
/// @brief GpPen, GpCustomLineCap / AdjustableArrowCap, and the stroker every
/// Draw* export (and GdipWidenPath) renders through.
///
/// Contract sources: Windows SDK GdiPlusFlat.h / GdiPlusPen.h /
/// GdiPlusLineCaps.h / GdiPlusEnums.h (LineCap, DashCap, DashStyle,
/// LineJoin, PenAlignment, PenType, CustomLineCapType), the Microsoft Learn
/// Pen / CustomLineCap / AdjustableArrowCap pages, and the winoracle probe
/// tools/oracle_probe/gdiplus_draw_probe.c (.expected beside it) for the
/// defaults, error codes and widened geometry Learn does not state.

#include "winapi/graphics/gdiplus/gdiplus_core.hpp"

#include <memory>
#include <vector>

typedef const struct CGPath* CGPathRef;

namespace vitreo::winapi::gdip {

class RenderScope;
struct GpPath;
struct GpBrush;

/// GDI+ `LineCap` (GdiPlusEnums.h).
enum LineCap : i32 {
    LineCapFlat = 0,
    LineCapSquare = 1,
    LineCapRound = 2,
    LineCapTriangle = 3,
    LineCapNoAnchor = 0x10,
    LineCapSquareAnchor = 0x11,
    LineCapRoundAnchor = 0x12,
    LineCapDiamondAnchor = 0x13,
    LineCapArrowAnchor = 0x14,
    LineCapCustom = 0xff,
    LineCapAnchorMask = 0xf0,
};

/// GDI+ `DashCap`.
enum DashCap : i32 { DashCapFlat = 0, DashCapRound = 2, DashCapTriangle = 3 };

/// GDI+ `DashStyle`.
enum DashStyle : i32 {
    DashStyleSolid = 0,
    DashStyleDash = 1,
    DashStyleDot = 2,
    DashStyleDashDot = 3,
    DashStyleDashDotDot = 4,
    DashStyleCustom = 5,
};

/// GDI+ `LineJoin`.
enum LineJoin : i32 {
    LineJoinMiter = 0,
    LineJoinBevel = 1,
    LineJoinRound = 2,
    LineJoinMiterClipped = 3,
};

/// GDI+ `PenAlignment`.
enum PenAlignment : i32 { PenAlignmentCenter = 0, PenAlignmentInset = 1 };

/// GDI+ `CustomLineCapType`.
enum CustomLineCapType : i32 {
    CustomLineCapTypeDefault = 0,
    CustomLineCapTypeAdjustableArrow = 1,
};

/// A CustomLineCap or AdjustableArrowCap. Cap coordinates are in units of
/// the pen width (times width_scale): the origin sits on the line's end
/// point and +Y points along the line, away from it.
struct GpCustomLineCap {
    i32 type{CustomLineCapTypeDefault};
    std::shared_ptr<GpPath> fill_path;    ///< null when none
    std::shared_ptr<GpPath> stroke_path;  ///< null when none
    i32 base_cap{LineCapFlat};
    float base_inset{0.0f};
    i32 stroke_start_cap{LineCapFlat};
    i32 stroke_end_cap{LineCapFlat};
    i32 stroke_join{LineJoinMiter};
    float width_scale{1.0f};
    // AdjustableArrowCap state; the paths above are regenerated from it.
    float arrow_height{0.0f};
    float arrow_width{0.0f};
    float arrow_middle_inset{0.0f};
    bool arrow_filled{true};
};

struct GpPen {
    float width{1.0f};
    i32 unit{2};  ///< GpUnit (UnitWorld..UnitMillimeter; never UnitDisplay)
    /// The fill. GdipCreatePen1 / SetPenColor make a SolidColor brush.
    std::shared_ptr<GpBrush> brush;
    i32 start_cap{LineCapFlat};
    i32 end_cap{LineCapFlat};
    i32 dash_cap{DashCapFlat};
    std::shared_ptr<GpCustomLineCap> custom_start;  ///< start_cap == LineCapCustom
    std::shared_ptr<GpCustomLineCap> custom_end;    ///< end_cap == LineCapCustom
    i32 line_join{LineJoinMiter};
    float miter_limit{10.0f};
    i32 alignment{PenAlignmentCenter};
    Affine transform;
    i32 dash_style{DashStyleSolid};
    float dash_offset{0.0f};
    std::vector<float> dash_array;  ///< in pen widths; empty for Solid
    std::vector<float> compound;    ///< empty, or 2n increasing values in [0,1]
};

/// The standard dash pattern for @p style (Learn DashStyle), in pen widths.
[[nodiscard]] std::vector<float> dash_pattern(i32 style);

/// Deep copy (GdipClonePen / GdipGetPenCustom*Cap clone their objects).
[[nodiscard]] std::shared_ptr<GpCustomLineCap> clone_cap(const GpCustomLineCap& cap);
[[nodiscard]] GpPen clone_pen(const GpPen& pen);

/// Stroke @p world_path (WORLD coordinates) with @p pen into @p scope: width
/// in the pen's unit (0 = one device pixel), pen transform, dashes and dash
/// caps, start/end/anchor/custom caps, joins and miter limit, alignment,
/// compound bands; the widened outline is painted with the pen's brush
/// through fill_path.
Status stroke_path(RenderScope& scope, const GpPath& world_path, const GpPen& pen);

/// The outline @p pen paints along @p path, as a path to fill with the
/// winding rule. @p m (null = identity) maps @p path to the space the pen
/// width is measured in; the result is in that space. Curves are
/// flattened to @p flatness first. Used by GdipWidenPath.
void widen_path(const GpPath& path, const GpPen& pen, const Affine* m, float flatness,
                GpPath& out);

/// Paint @p path (WORLD coordinates) with @p brush, as fill_path, but with
/// GDI+'s aliased rasterization when the graphics is not antialiased: a
/// pixel is painted when its centre lies inside (left/top edges in,
/// right/bottom edges out; probe fillrect-*, line-*). CoreGraphics' own
/// aliased fill paints every pixel the shape touches instead.
Status paint_path(RenderScope& scope, CGPathRef path, bool even_odd, const GpBrush& brush);

/// Status for a handle argument: NULL -> InvalidParameter; @p found -> Ok;
/// a live GDI+ object of another class -> ObjectBusy (probe: a brush, pen
/// or graphics passed where another class is expected); else
/// InvalidParameter.
[[nodiscard]] Status resolve_status(GdiplusManager& mgr, u64 handle, bool found);

/// Pen, CustomLineCap and AdjustableArrowCap exports (called by register_draw).
void register_pen(GdiplusManager& mgr, HandlerMap& map);

}  // namespace vitreo::winapi::gdip
