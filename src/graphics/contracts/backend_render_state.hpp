// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_render_state.hpp
/// @brief Fixed-function render state shared by every guest API frontend.

#include <vitreo/common.hpp>
#include <cstring>
#include <limits>

namespace vitreo::graphics {

// =============================================================================
// Rasterization contract
// =============================================================================
//
// Guest graphics APIs describe drawing very differently, but they all reduce to
// the same request: bind a pipeline built from shaders and a vertex layout, and
// issue a draw against some buffers and textures. The types below are that
// common request. Shaders arrive as MSL because the shared shader layer
// (`graphics/core/shader/`) is what converts DXBC, SPIR-V, GLSL, and the D3D9
// fixed-function state into MSL; the backend consumes only its output.

class BackendBuffer;
class BackendTexture2D;
class BackendTexture3D;
class BackendTextureBuffer;
class BackendHeap;

enum class BackendPrimitiveTopology {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
};

/// Tessellator spacing (Metal MTLTessellationPartitionMode; D3D11
/// dcl_tessellator_partitioning).
enum class BackendTessellationPartition : u8 {
    Integer,
    Pow2,
    FractionalOdd,
    FractionalEven,
};

/// The primitive CLASS a render pipeline rasterizes. Metal requires it on a
/// pipeline whose vertex stage writes the render-target array index (layered
/// rendering, D3D SV_RenderTargetArrayIndex from a vertex shader); every
/// other pipeline may leave it Unspecified.
enum class BackendPrimitiveClass : u8 {
    Unspecified,
    Point,
    Line,
    Triangle,
};

[[nodiscard]] constexpr BackendPrimitiveClass primitive_class_of(
    BackendPrimitiveTopology topology) noexcept {
    switch (topology) {
    case BackendPrimitiveTopology::PointList:
        return BackendPrimitiveClass::Point;
    case BackendPrimitiveTopology::LineList:
    case BackendPrimitiveTopology::LineStrip:
        return BackendPrimitiveClass::Line;
    case BackendPrimitiveTopology::TriangleList:
    case BackendPrimitiveTopology::TriangleStrip:
        return BackendPrimitiveClass::Triangle;
    }
    return BackendPrimitiveClass::Unspecified;
}

enum class BackendVertexFormat {
    Float1,
    Float2,
    Float3,
    Float4,
    /// Four bytes normalized to 0..1, in BGRA order — the layout of a
    /// D3DCOLOR vertex element.
    UByte4NormalizedBGRA,
    /// DXGI_FORMAT_R8G8B8A8_UNORM vertex elements (RGBA memory order).
    UByte4Normalized,
    /// D3DDECLTYPE_UBYTE4 — four unsigned bytes, not normalized.
    UByte4,
    /// D3DDECLTYPE_SHORT2 / SHORT4 — signed 16-bit integers.
    Short2,
    Short4,
    /// D3DDECLTYPE_SHORT2N / SHORT4N — signed 16-bit normalized.
    Short2Normalized,
    Short4Normalized,
    /// D3DDECLTYPE_USHORT2N / USHORT4N — unsigned 16-bit normalized.
    UShort2Normalized,
    UShort4Normalized,
    /// D3DDECLTYPE_FLOAT16_2 / FLOAT16_4 — IEEE half floats.
    Half2,
    Half4,
    /// D3DDECLTYPE_DEC3N — packed signed 10:10:10 (:2) normalized.
    Dec3N,
    /// DXGI_FORMAT_R16G16_UINT — two unsigned 16-bit integers, not normalized.
    UShort2,
    /// Sentinel used when a guest vertex format has no native backend form.
    Invalid,
    UInt1,
    UInt2,
    UInt3,
    UInt4,
    SInt1,
    SInt2,
    SInt3,
    SInt4,
    Half1,
    UShort1,
    UShort4,
    Short1,
    UByte1,
    UByte2,
    SByte1,
    SByte2,
    SByte4,
    UByte1Normalized,
    UByte2Normalized,
    SByte1Normalized,
    SByte2Normalized,
    SByte4Normalized,
    UShort1Normalized,
    Short1Normalized,
    UInt1010102Normalized,
    FloatRG11B10,
    FloatRGB9E5,
    /// DXGI_FORMAT_R10G10B10A2_UINT: four unsigned integers packed 10:10:10:2.
    /// Metal has no integer form of this packing, so it is fetched as
    /// UInt1010102Normalized and the vertex shader restores the integers
    /// (DxbcTranslationOptions::vertex_input_unpack_uint1010102).
    UInt1010102,
};

/// Scalar type presented by the native vertex fetch stage. Normalized and
/// floating storage produce Float; unnormalized integer storage retains its
/// signedness. Frontends also carry the shader signature type so a pipeline
/// compiler can bridge legal D3D layout/signature mismatches explicitly.
enum class BackendVertexComponentType { Float, UInt, SInt };

enum class BackendCullMode { None, Front, Back };
enum class BackendFillMode { Fill, Lines };

enum class BackendCompare {
    Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always,
};

enum class BackendStencilOperation {
    Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert,
    IncrementWrap, DecrementWrap,
};

struct BackendStencilFaceDesc {
    BackendStencilOperation stencil_fail{BackendStencilOperation::Keep};
    BackendStencilOperation depth_fail{BackendStencilOperation::Keep};
    BackendStencilOperation pass{BackendStencilOperation::Keep};
    BackendCompare compare{BackendCompare::Always};
};

enum class BackendBlendFactor {
    Zero, One,
    SourceColor, OneMinusSourceColor,
    SourceAlpha, OneMinusSourceAlpha,
    DestinationColor, OneMinusDestinationColor,
    DestinationAlpha, OneMinusDestinationAlpha,
    SourceAlphaSaturated,
    BlendColor, OneMinusBlendColor,
    Source1Color, OneMinusSource1Color,
    Source1Alpha, OneMinusSource1Alpha,
};

enum class BackendBlendOperation { Add, Subtract, ReverseSubtract, Min, Max };

enum class BackendFilter { Nearest, Linear };

enum class BackendAddressMode { Wrap, Mirror, Clamp, Border, MirrorOnce };

struct BackendVertexAttribute {
    /// Attribute index the shader declares with `[[attribute(n)]]`.
    unsigned shader_location{0};
    unsigned offset{0};
    BackendVertexFormat format{BackendVertexFormat::Float4};
    /// D3D input slot / Metal vertex-buffer stream index (before backend base).
    unsigned buffer_index{0};
    BackendVertexComponentType storage_component_type{
        BackendVertexComponentType::Float};
    BackendVertexComponentType shader_component_type{
        BackendVertexComponentType::Float};
};

struct BackendSamplerDesc {
    BackendFilter min_filter{BackendFilter::Linear};
    BackendFilter mag_filter{BackendFilter::Linear};
    BackendFilter mip_filter{BackendFilter::Nearest};
    /// When false, sample level 0 only (`D3DTEXF_NONE`). Mapping NONE to
    /// Nearest mip incorrectly pulls distant LODs (blocky UI / green mips).
    bool mipmaps_enabled{true};
    BackendAddressMode address_u{BackendAddressMode::Wrap};
    BackendAddressMode address_v{BackendAddressMode::Wrap};
    BackendAddressMode address_w{BackendAddressMode::Wrap};
    unsigned max_anisotropy{1};
    // No reduction mode: D3D11 MINIMUM_ / MAXIMUM_ filters reduce in the
    // translated shader (DxbcTranslationOptions::minmax_samplers). Metal's
    // MTLSamplerDescriptor.reductionMode returned the weighted average on an
    // M4 Pro (macOS 26.3) and fails Metal API validation.
    /// Comparison samplers perform the D3D SampleCmp/SAMPLE_C test in the
    /// sampler. Ordinary samplers leave this disabled.
    bool comparison_enabled{false};
    BackendCompare comparison{BackendCompare::Never};
    float lod_min_clamp{0.0f};
    float lod_max_clamp{std::numeric_limits<float>::max()};
    float lod_bias{0.0f};
    /// Guest border color as D3DCOLOR (`0xAARRGGBB`). Metal only supports
    /// TransparentBlack / OpaqueBlack / OpaqueWhite; backends quantize to the
    /// nearest of those three at sampler creation time.
    u32 border_color{0};
};

/// Whether two sampler descs describe the same sampler, field by field.
[[nodiscard]] inline bool same_sampler_desc(const BackendSamplerDesc& a,
                                            const BackendSamplerDesc& b) noexcept {
    return a.min_filter == b.min_filter && a.mag_filter == b.mag_filter &&
           a.mip_filter == b.mip_filter &&
           a.mipmaps_enabled == b.mipmaps_enabled &&
           a.address_u == b.address_u && a.address_v == b.address_v &&
           a.address_w == b.address_w && a.max_anisotropy == b.max_anisotropy &&
           a.comparison_enabled == b.comparison_enabled &&
           a.comparison == b.comparison &&
           std::memcmp(&a.lod_min_clamp, &b.lod_min_clamp, sizeof(float)) == 0 &&
           std::memcmp(&a.lod_max_clamp, &b.lod_max_clamp, sizeof(float)) == 0 &&
           std::memcmp(&a.lod_bias, &b.lod_bias, sizeof(float)) == 0 &&
           a.border_color == b.border_color;
}

/// native-state (delta_record.hpp, R3c): a sampler built once from desc(),
/// when the API object is created (D3D11 CreateSamplerState), so a draw can
/// name it instead of having its desc looked up. The native state it stands
/// for lives as long as the backend, so a draw recorded with it may outlive
/// this object.
class BackendSamplerState {
public:
    virtual ~BackendSamplerState() = default;
    [[nodiscard]] virtual const BackendSamplerDesc& desc() const noexcept = 0;
};

/// A draw's depth and stencil state: exactly the depth/stencil fields of
/// BackendDrawCall (after any read-only depth-stencil view masking).
struct BackendDepthStencilDesc {
    bool depth_test_enabled{false};
    bool depth_write_enabled{false};
    BackendCompare depth_compare{BackendCompare::LessEqual};
    bool stencil_test_enabled{false};
    u8 stencil_read_mask{0xFF};
    u8 stencil_write_mask{0xFF};
    BackendStencilFaceDesc front_stencil{};
    BackendStencilFaceDesc back_stencil{};
};

[[nodiscard]] inline bool same_stencil_face(const BackendStencilFaceDesc& a,
                                            const BackendStencilFaceDesc& b) noexcept {
    return a.stencil_fail == b.stencil_fail && a.depth_fail == b.depth_fail &&
           a.pass == b.pass && a.compare == b.compare;
}

[[nodiscard]] inline bool same_depth_stencil_desc(
    const BackendDepthStencilDesc& a, const BackendDepthStencilDesc& b) noexcept {
    return a.depth_test_enabled == b.depth_test_enabled &&
           a.depth_write_enabled == b.depth_write_enabled &&
           a.depth_compare == b.depth_compare &&
           a.stencil_test_enabled == b.stencil_test_enabled &&
           a.stencil_read_mask == b.stencil_read_mask &&
           a.stencil_write_mask == b.stencil_write_mask &&
           same_stencil_face(a.front_stencil, b.front_stencil) &&
           same_stencil_face(a.back_stencil, b.back_stencil);
}

/// native-state (R3c): a depth-stencil state built once from desc(). As
/// BackendSamplerState, its native state outlives it.
class BackendDepthStencilState {
public:
    virtual ~BackendDepthStencilState() = default;
    [[nodiscard]] virtual const BackendDepthStencilDesc& desc() const noexcept = 0;
};

} // namespace vitreo::graphics
