// SPDX-License-Identifier: LGPL-2.1-only
/// @file raster_emulation_types.hpp
/// @brief The contract between the raster-emulation pass C vertex shader
///        (raster_emulation.hpp) and the pixel shaders translated for it
///        (DxbcTranslationOptions::raster_emulation).
///
/// Metal rasterizes a primitive only at the sample count of the pass it
/// draws into and never conservatively. Two D3D rasterizer modes need more:
///
///   - target-independent rasterization (D3D11.3 functional spec 3.5.6,
///     D3D12_RASTERIZER_DESC::ForcedSampleCount): coverage of the N-sample
///     standard pattern whatever the targets' own sample count;
///   - conservative rasterization (DirectX-Specs
///     ConservativeRasterization.md): every pixel the primitive overlaps at
///     all, with SV_InnerCoverage for the pixels it covers entirely.
///
/// Both are emulated the same way. Pass C draws each triangle as a region
/// that contains every candidate pixel's centre (the triangle grown by the
/// pixel's half-extent, in the triangle's own plane so attributes keep
/// interpolating on it), and hands the pixel shader the triangle's 16.8
/// snapped edge functions as flat varyings. The pixel shader then evaluates
/// the exact D3D coverage itself -- sample by sample with the top-left rule
/// for target-independent rasterization, pixel square against triangle for
/// conservative rasterization -- discards pixels with none, and reports it
/// as SV_Coverage / SV_InnerCoverage.

#pragma once

#include "shader_types.hpp"

#include <array>

namespace vitreo::graphics {

/// One sample of a standard pattern: its offset from the pixel centre in
/// 1/16 pixel (D3D11.3 19.2.4, "a sub-pixel grid limited to 16 horizontal
/// and vertical divisions"), x right, y down.
struct StandardSample {
    i8 x;
    i8 y;
};

/// The D3D standard sample patterns (D3D11_STANDARD_MULTISAMPLE_PATTERN),
/// sample index order. Measured on Windows: Texture2DMS::GetSamplePosition
/// of D3D12 textures at each count (WARP and an RTX 4080 SUPER agree; the
/// RTX has no 16x RGBA8 texture, WARP does), d3d12_a31_raster_probe
/// `samplepos texture Nx` rows in tests/oracle_data. The TIR coverage rows
/// of the same probe follow these positions exactly
/// (RasterPatternMatchesOracle test).
inline constexpr std::array<StandardSample, 1> kStandardPattern1{{{0, 0}}};
inline constexpr std::array<StandardSample, 2> kStandardPattern2{
    {{4, 4}, {-4, -4}}};
inline constexpr std::array<StandardSample, 4> kStandardPattern4{
    {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}}};
inline constexpr std::array<StandardSample, 8> kStandardPattern8{
    {{1, -3}, {-1, 3}, {5, 1}, {-3, -5}, {-5, 5}, {-7, -1}, {3, 7}, {7, -7}}};
inline constexpr std::array<StandardSample, 16> kStandardPattern16{
    {{1, 1}, {-1, -3}, {-3, 2}, {4, -1}, {-5, -2}, {2, 5}, {5, 3}, {3, -5},
     {-2, 6}, {0, -7}, {-4, -6}, {-6, 4}, {-8, 0}, {7, -4}, {6, 7},
     {-7, -8}}};

/// The standard pattern of @p count samples (1, 2, 4, 8, 16), else nullptr.
[[nodiscard]] constexpr const StandardSample* standard_sample_pattern(
    u32 count) {
    switch (count) {
    case 1: return kStandardPattern1.data();
    case 2: return kStandardPattern2.data();
    case 4: return kStandardPattern4.data();
    case 8: return kStandardPattern8.data();
    case 16: return kStandardPattern16.data();
    default: return nullptr;
    }
}

enum class RasterEmulationMode : u32 {
    Off = 0,
    /// Coverage of the forced_samples standard pattern (ForcedSampleCount).
    TargetIndependent = 1,
    /// Overestimated conservative rasterization, tier 3 behaviour: no
    /// post-snap degenerate is culled, a 1/512-pixel uncertainty region,
    /// SV_InnerCoverage.
    Conservative = 2,
};

/// What pass C rasterizes for one D3D primitive.
enum class RasterPrimitive : u32 {
    /// A triangle: three edges.
    Triangle = 0,
    /// A convex quadrilateral rasterized with triangle rules: a point ("a
    /// square of width 1 oriented to the RenderTarget", D3D11.3 3.4.6) or a
    /// quadrilateral line ("a simple rectangle with width 1.4f", 3.4.5;
    /// D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW: width 1.0), under
    /// target-independent rasterization (conservative rasterization of lines
    /// and points is undefined, ConservativeRasterization.md, and the
    /// runtime refuses every pipeline that would rasterize one with it,
    /// d3d12_a66_c6_probe). Four edges: edge 3 at kRasterEdge3Location.
    Quad = 1,
    /// An aliased line under target-independent rasterization (D3D11.3
    /// 3.4.3 diamond-exit rule; 15.14.1: "when the line covers a pixel ...
    /// all of the MSAA samples in the pixel are hit"). Edge 0 holds the
    /// snapped endpoints (A.x, A.y, B.x, B.y) in 1/512 pixel.
    AliasedLine = 2,
};

/// Pixel-shader side (DxbcTranslationOptions::raster_emulation).
struct RasterEmulationOptions {
    RasterEmulationMode mode{RasterEmulationMode::Off};
    RasterPrimitive primitive{RasterPrimitive::Triangle};
    /// The edge data does not fit in Metal's fragment inputs with the
    /// pixel shader's own (124 components, Metal Feature Set Tables): it is
    /// read from the edge buffer instead, kRasterEdgeBufferSlots int4 per
    /// primitive at Metal primitive id / kRasterTrianglesPerPrimitive
    /// (SPIRV_STATISTICS_DESCRIPTOR_SET binding 0, the fragment statistics
    /// index -- a raster-emulated draw never runs the counting translation).
    bool edges_in_buffer{false};
    /// TargetIndependent: the pattern's sample count N (1, 4, 8, 16), the
    /// bits of SV_Coverage. Conservative: the samples a covered pixel
    /// reports in SV_Coverage -- ForcedSampleCount when set, else the
    /// targets' sample count ("the first n bits ... given an n sample per
    /// pixel RenderTarget ... or an n sample ForcedSampleCount").
    u32 samples{1};
    /// Conservative: the rasterizer state's SampleMask, ANDed into
    /// SV_Coverage ("the SampleMask Rasterizer State applies the same way"),
    /// never into SV_InnerCoverage.
    u32 sample_mask{0xFFFFFFFFu};
};

/// Fragment input locations of the per-triangle data pass C writes (flat),
/// past v0..v31 and kLineDistanceLocation (32):
///   +0..+2  int4 edge i: (A, B, C low 32 bits, C high 32 bits). The
///           edge function A*X + B*Y + C over X, Y in 1/512 pixel (window
///           coordinates, y down) is >= 0 inside. Target-independent
///           rasterization folds the top-left rule into C (a sample on an
///           edge that is not top or left is outside).
///   +3      int4 bounding box (min X, min Y, max X, max Y), 1/512 pixel.
///   +4      float4 (a, b, c, flags): 1/w = a*x + b*y + c over window
///           pixels; flags = kRasterTriangle* bits (as_type<float>).
///   +5      float4 (a, b, c, 0): z/w = a*x + b*y + c, for the depth clip.
inline constexpr u32 kRasterEdgeLocation = 33;
inline constexpr u32 kRasterBoundsLocation = 36;
inline constexpr u32 kRasterInversePlaneLocation = 37;
inline constexpr u32 kRasterDepthPlaneLocation = 38;
inline constexpr u32 kRasterEmulationLocations = 6;
/// SV_Barycentrics of the D3D primitive at the fragment: pass C writes each
/// region vertex's weights over the primitive's vertices (float4, w 0) at
/// these locations, perspective-interpolated and noperspective
/// respectively; the pixel shader reads them instead of
/// [[barycentric_coord]] (DxbcToSpirvResult::raster_barycentrics). Past the
/// GetAttributeAtVertex copies (per_vertex_input_location, up to 135).
inline constexpr u32 kRasterBaryLocation = 39;
inline constexpr u32 kRasterBaryNoPerspectiveLocation = 136;
/// RasterPrimitive::Quad: the fourth edge (int4, as +0..+2).
inline constexpr u32 kRasterEdge3Location = 137;
/// Metal's limit on a fragment function's inputs: 124 scalar components
/// (Metal Feature Set Tables, the fragment function input limit). A
/// raster-emulated pixel shader
/// whose own inputs leave no room for pass C's edge data reads it from the
/// edge buffer (RasterEmulationOptions::edges_in_buffer).
inline constexpr u32 kMaxFragmentInputComponents = 124;

/// Per-triangle flags (location +4, w).
/// Clip to 0 <= z/w <= 1 (DepthClipEnable).
inline constexpr u32 kRasterTriangleDepthClip = 1u << 0;
/// The triangle is degenerate after snapping: a segment or a point (edge 0
/// and 1 are its line, both ways; edge 2 is empty). No InnerCoverage.
inline constexpr u32 kRasterTriangleDegenerate = 1u << 1;
/// A vertex has w <= 0: only where 1/w > 0 is inside.
inline constexpr u32 kRasterTriangleClipW = 1u << 2;

}  // namespace vitreo::graphics
