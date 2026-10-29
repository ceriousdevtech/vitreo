#pragma once
/// @file gdiplus_graphics.hpp
/// @brief GpGraphics: a drawing target plus the rendering state the
/// GdipSet* / GdipGet* graphics exports read and write.
///
/// Contract: GdiPlusFlat.h / GdiPlusEnums.h / GdiPlusInit.h, the Learn
/// Graphics class pages, and tools/oracle_probe/gdiplus_graphics_probe.c
/// (.expected / .x86.expected) for every value Learn does not state. Notes:
/// .docs/contracts/gdiplus/graphics.md.

#include "winapi/graphics/gdiplus/gdiplus_core.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace vitreo::winapi::gdip {

struct GpRegion;  // gdiplus_region.hpp

/// GDI+ `Unit` (GdiPlusEnums.h).
enum Unit : i32 {
    UnitWorld = 0, UnitDisplay = 1, UnitPixel = 2, UnitPoint = 3,
    UnitInch = 4, UnitDocument = 5, UnitMillimeter = 6,
};

/// The state Save/Restore and BeginContainer/EndContainer capture.
struct GraphicsState {
    Affine world;                    ///< GdipSetWorldTransform
    i32 page_unit{UnitDisplay};
    float page_scale{1.0f};
    /// The clip, kept in DEVICE space (GDI+ transforms a clip when it is
    /// set, so a later world-transform change does not move it on the
    /// page). Null = infinite.
    std::shared_ptr<GpRegion> clip;
    // Fresh-graphics values (gdiplus_graphics_probe "hdc fresh"/"img fresh"):
    // SmoothingModeNone (3) and InterpolationModeBilinear (3), as the
    // getters report them; the setters store Default/HighSpeed/HighQuality
    // as these resolved modes.
    i32 compositing_mode{0};         ///< CompositingModeSourceOver
    i32 compositing_quality{0};      ///< CompositingQualityDefault
    i32 smoothing_mode{3};           ///< SmoothingModeNone
    i32 interpolation_mode{3};       ///< InterpolationModeBilinear
    i32 pixel_offset_mode{0};        ///< PixelOffsetModeDefault
    i32 text_rendering_hint{0};      ///< TextRenderingHintSystemDefault
    u32 text_contrast{4};
    i32 rendering_origin_x{0};
    i32 rendering_origin_y{0};
    /// Container base: maps this level's page-device pixels (world, then
    /// page unit/scale/pixel offset) to the target's device pixels. At the
    /// top level it is the DC's logical->device transform captured when
    /// the graphics was created from an HDC (identity for an image); a
    /// container composes the enclosing levels into it (see
    /// world_to_device).
    Affine container_world;
    /// The intersection of every enclosing level's clip (device space),
    /// null when infinite. Drawing clips to it and to @c clip.
    std::shared_ptr<GpRegion> container_clip;
};

/// What a GpGraphics draws into.
enum class TargetKind { Image, Hdc, Hwnd };

/// One GdipSaveGraphics / GdipBeginContainer entry. Both share one stack:
/// GdipRestoreGraphics only matches a Save entry and GdipEndContainer only
/// a Container entry; either pops every entry above the match
/// (gdiplus_graphics_probe "mix ...").
struct SavedGraphicsState {
    u32 token{0};
    bool container{false};
    GraphicsState state;
};

struct GpGraphics {
    TargetKind kind{TargetKind::Image};
    u64 image{0};        ///< GpImage handle (TargetKind::Image)
    u64 hdc{0};          ///< HDC (Hdc), or the DC GetDC'd for Hwnd
    u64 hwnd{0};
    bool owns_hdc{false};  ///< GdipCreateFromHWND: release on delete
    float dpi_x{96.0f};
    float dpi_y{96.0f};
    GraphicsState state;
    std::vector<SavedGraphicsState> saved;
    u32 next_state_token{1};
    bool hdc_out{false};  ///< between GdipGetDC and GdipReleaseDC
    /// GdipGetDC on an image target: the memory DC and 32bpp DIB section
    /// handed out, released (and written back) by GdipReleaseDC.
    u64 image_dc{0};
    u64 image_dc_bitmap{0};
    /// GdipGraphicsSetAbort: the guest GdiplusAbort* (0 = none).
    u64 abort_object{0};
};

/// Device units per one unit of @p unit at @p dpi (UnitWorld/UnitDisplay:
/// 1 for a display or image target, gdiplus_graphics_probe "hdc unit1").
[[nodiscard]] float unit_to_device(i32 unit, float dpi);
/// Page transform of @p g's current level: page unit * page scale, then
/// the PixelOffsetModeHalf/HighQuality -0.5 shift.
[[nodiscard]] Affine page_to_container(const GpGraphics& g);
/// World -> device: world transform, page transform, then the container
/// base (which includes the DC transform). TransformPoints and every draw
/// use exactly this.
[[nodiscard]] Affine world_to_device(const GpGraphics& g);
/// Inverse of @p a; false when it is singular.
[[nodiscard]] bool affine_invert(const Affine& a, Affine& out);

/// The status a graphics export answers before touching @p g: null ->
/// InvalidParameter; a DC out through GdipGetDC -> ObjectBusy (probe
/// "... while out"). Ok otherwise.
[[nodiscard]] Status graphics_usable(const GpGraphics* g);

/// True between a successful GdiplusStartup and its last GdiplusShutdown.
/// GDI+ answers GdiplusNotInitialized from every export outside that
/// window (probe "pre-startup", "after shutdown").
[[nodiscard]] bool gdiplus_started(const GdiplusManager& mgr);

} // namespace vitreo::winapi::gdip
