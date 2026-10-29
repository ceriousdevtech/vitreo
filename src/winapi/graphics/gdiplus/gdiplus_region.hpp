#pragma once
/// @file gdiplus_region.hpp
/// @brief GpRegion (the GDI+ Region object) and its device-pixel raster,
/// shared by the Region exports, the graphics clip and the renderer.
///
/// A region is GDI+'s tree: rect / path / empty / infinite leaves under
/// combine nodes, kept exactly as the caller built it (GetRegionData
/// serializes that tree). Every pixel question — bounds of a combined
/// region, scans, IsEmpty / IsEqual / IsInfinite, IsVisible, HRGN
/// conversion, FillRegion, clipping — is answered from the region's
/// raster in device space: pixel (x, y) is in the region when the point
/// (x, y) is (GDI+ pixel centres sit on integer coordinates), edges
/// left/top-inclusive, right/bottom-exclusive, coordinates snapped to the
/// rasterizer's 1/16-pixel grid, and the infinite region is the square
/// [-4194304, 4194304)^2 whatever the transform.
///
/// Contract sources: GdiPlusFlat.h, GdiPlusRegion.h, GdiPlusEnums.h
/// (CombineMode), the Microsoft Learn Region / Graphics clip pages, and
/// the winoracle probe tools/oracle_probe/gdiplus_region_probe.c
/// (gdiplus_region_probe{32,64}.expected) for the serialized format, the
/// raster rules and every simplification below.

#include "winapi/graphics/gdiplus/gdiplus_core.hpp"
#include "winapi/graphics/gdiplus/gdiplus_geometry.hpp"

#include <array>
#include <memory>
#include <vector>

typedef struct CGContext* CGContextRef;
typedef struct CGPath* CGMutablePathRef;

namespace vitreo::winapi::gdip {

/// GDI+ `CombineMode` (GdiPlusEnums.h). The serialized combine node uses
/// the same values (1..5).
enum CombineMode : i32 {
    CombineModeReplace = 0,
    CombineModeIntersect = 1,
    CombineModeUnion = 2,
    CombineModeXor = 3,
    CombineModeExclude = 4,     ///< this minus operand
    CombineModeComplement = 5,  ///< operand minus this
};

/// The device-pixel square GDI+ treats as infinite (GetRegionBounds of an
/// infinite region: -4194304, -4194304, 8388608, 8388608).
inline constexpr i32 kRegionInfiniteMin = -4194304;
inline constexpr i32 kRegionInfiniteMax = 4194304;

/// One horizontal band of a raster: rows [y0, y1), covered columns as
/// sorted half-open pairs xs = {x0, x1, x0', x1', ...}.
struct ScanBand {
    i32 y0{0};
    i32 y1{0};
    std::vector<i32> xs;
    bool operator==(const ScanBand&) const = default;
};

/// A region rasterized to device pixels, in canonical GDI band form
/// (bands sorted, non-empty, vertically coalesced; spans disjoint and
/// non-adjacent), so equal pixel sets compare equal.
struct RegionScans {
    std::vector<ScanBand> bands;

    [[nodiscard]] bool empty() const { return bands.empty(); }
    [[nodiscard]] static RegionScans rect(i32 x0, i32 y0, i32 x1, i32 y1);
    [[nodiscard]] static RegionScans infinite();
    [[nodiscard]] bool contains(i32 x, i32 y) const;
    /// Bounding box as x0, y0, x1, y1; false (and zeros) when empty.
    bool bounds(i32& x0, i32& y0, i32& x1, i32& y1) const;
    /// The rectangles GetRegionScans reports: one per span per band.
    [[nodiscard]] usize rect_count() const;
    [[nodiscard]] bool is_infinite() const;
    bool operator==(const RegionScans&) const = default;
};

/// @p a combined with @p b by a CombineMode (Replace yields @p b).
[[nodiscard]] RegionScans scans_combine(const RegionScans& a, const RegionScans& b, i32 mode);

struct GpRegion {
    /// Node kinds; the leaf values are the serialized node types.
    enum class Kind : u32 {
        Combine = 0,
        Rect = 0x10000000,
        Path = 0x10000001,
        Empty = 0x10000002,
        Infinite = 0x10000003,
    };
    Kind kind{Kind::Infinite};   ///< GdipCreateRegion: infinite
    i32 mode{0};                 ///< Combine: CombineModeIntersect..Complement
    RectF rect;                  ///< Rect: as given (a negative extent is kept)
    GpPath path;                 ///< Path: a private copy of the caller's path
    std::shared_ptr<GpRegion> left;   ///< Combine: this ...
    std::shared_ptr<GpRegion> right;  ///< ... combined with this
    /// Raster under the identity transform (the clip asks this repeatedly).
    /// Every mutation through the functions below resets it.
    mutable std::shared_ptr<const RegionScans> identity_scans;
};

/// Apply @p region (in device space) as the clip of @p cg: its raster as
/// rectangles in the current CTM. Null = infinite. Declared also in
/// gdiplus_render.hpp.
void clip_to_region(CGContextRef cg, const GpRegion* region);
/// The region's raster as a path of pixel squares in canvas coordinates
/// (device pixel i spans [i, i + 1), the space clip_to_region clips in,
/// before RenderScope's half-pixel shift); null for an infinite region.
/// Caller releases with CGPathRelease.
[[nodiscard]] CGMutablePathRef region_to_cg(const GpRegion& region);
/// A deep copy (no node shared with @p region).
[[nodiscard]] std::shared_ptr<GpRegion> region_clone(const GpRegion& region);
/// GdipTransformRegion: an axis-aligned, non-degenerate matrix keeps a rect
/// a (normalized) rect; any other turns it into a 4-point path; empty and
/// infinite are unchanged.
void region_transform(GpRegion& region, const Affine& m);
/// GdipTranslateRegion: offsets rects and path points; empty/infinite unchanged.
void region_translate(GpRegion& region, float dx, float dy);
/// Combine @p operand into @p target as GDI+ does: Replace copies it, an
/// empty or infinite operand / target folds per mode, anything else becomes
/// a combine node (no geometric simplification).
void region_combine(GpRegion& target, const GpRegion& operand, i32 mode);
/// Raster of @p region after @p to_device.
[[nodiscard]] RegionScans region_raster(const GpRegion& region, const Affine& to_device);
/// The identity raster, cached on the region.
[[nodiscard]] const RegionScans& region_device_scans(const GpRegion& region);
/// Raster of @p rect after @p to_device (empty for a non-positive extent).
[[nodiscard]] RegionScans rect_raster(const RectF& rect, const Affine& to_device);
/// Raster of @p path (its fill mode) after @p to_device.
[[nodiscard]] RegionScans path_raster(const GpPath& path, const Affine& to_device);
/// GdipGetRegionData's byte stream.
[[nodiscard]] std::vector<u8> region_serialize(const GpRegion& region);
/// GdipCreateRegionRgnData's parser: false for a stream GDI+ rejects.
[[nodiscard]] bool region_deserialize(const u8* data, usize size, GpRegion& out);
/// A region of the rectangles of a GDI region, as GdipCreateRegionHrgn
/// builds it: one closed 4-point figure per rectangle, winding fill.
[[nodiscard]] GpRegion region_from_rects(const std::vector<std::array<i32, 4>>& ltrb);

}  // namespace vitreo::winapi::gdip
