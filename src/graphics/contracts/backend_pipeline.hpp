// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_pipeline.hpp
/// @brief Render and compute pipeline descriptors and shader-visible texture views.

#include "graphics/contracts/backend_render_state.hpp"
#include "graphics/contracts/format_converter.hpp"
#include <array>
#include <string>
#include <vector>

namespace vitreo::graphics {

constexpr unsigned BACKEND_MAX_RENDER_TARGETS = 8;
/// Viewports (and scissor rects) a draw may index: D3D11's
/// D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, and Metal's.
constexpr unsigned BACKEND_MAX_VIEWPORTS = 16;

/// Floating-point optimisation contract a translated shader is compiled
/// under. Fast is Metal's default (no INF/NaN, reassociation); Relaxed allows
/// reassociation but "preserves infs and nans"; Safe allows neither
/// (Metal SDK MTLLibrary.h, MTLMathMode). D3D11.3 3.1.3.1 requires INF/NaN
/// semantics for every shader and 22.3.2 forbids refactoring unless the
/// shader declares refactoringAllowed and the instruction is not precise.
enum class BackendMathMode : u8 { Fast, Relaxed, Safe };

struct BackendPipelineDesc {
    std::string vertex_shader_msl;
    std::string fragment_shader_msl;
    BackendMathMode vertex_math_mode{BackendMathMode::Fast};
    BackendMathMode fragment_math_mode{BackendMathMode::Fast};
    /// Compile the vertex function so an `[[invariant]]` position is computed
    /// identically in every pipeline (MTLCompileOptions.preserveInvariance).
    bool preserve_invariance{false};
    std::string vertex_entry_point{"vertex_main"};
    std::string fragment_entry_point{"fragment_main"};

    std::vector<BackendVertexAttribute> vertex_attributes;
    /// Stride per D3D input slot (index = BackendVertexAttribute::buffer_index).
    std::array<unsigned, 8> vertex_buffer_strides{};
    /// Per-input-slot fetch classification. False advances per vertex; true
    /// advances once per `vertex_buffer_step_rates[slot]` instances. A step
    /// rate of 0 never advances: every instance reads the element at
    /// StartInstanceLocation (D3D11.3 8.3.1 pseudocode; measured,
    /// tools/oracle_probe/d3d11_a07_ia_probe.expected `B step=0`).
    std::array<bool, 8> vertex_buffer_per_instance{};
    std::array<unsigned, 8> vertex_buffer_step_rates{};
    /// The guest bound this slot with stride 0: every vertex AND every
    /// instance fetches the element at the binding offset, whatever the
    /// slot's classification, step rate or StartInstanceLocation (measured,
    /// tools/oracle_probe/d3d11_instance_id_probe.expected). Overrides the
    /// slot's stride and step function.
    std::array<bool, 8> vertex_buffer_constant{};
    /// Legacy single-stream stride (slot 0). Prefer vertex_buffer_strides[0].
    unsigned vertex_stride{0};

    /// False builds a pipeline whose vertex stage runs but whose primitives
    /// are discarded before rasterization. Pass A of the geometry-shader
    /// emulation needs this: the vertex shader runs only to write its outputs
    /// into the expansion buffer, and rasterising them would draw the
    /// unexpanded geometry over the frame. Metal wants no fragment function
    /// on such a pipeline, so the backend drops it.
    ///
    /// A caller that varies this MUST vary `cache_key` with it.
    bool rasterization_enabled{true};
    /// Rasterizes with no colour or depth attachment (see
    /// BackendRenderPassDesc::attachmentless_width); `sample_count` must
    /// match the pass's attachmentless_samples. Vary `cache_key` with it.
    bool attachmentless{false};

    PixelFormat color_format{PixelFormat::Unknown};
    /// Per-MRT attachment formats. Slot 0 falls back to `color_format` for
    /// existing single-target frontends.
    std::array<PixelFormat, BACKEND_MAX_RENDER_TARGETS> color_formats{};
    bool depth_attachment{false};
    /// The depth attachment's format. Unknown keeps the historical
    /// combined depth+stencil default. Vary `cache_key` with it.
    PixelFormat depth_format{PixelFormat::Unknown};
    /// D3D11_BLEND_DESC::AlphaToCoverageEnable (D3D11.3 17.18): render
    /// target 0's alpha becomes a coverage mask ANDed with the pixel's.
    bool alpha_to_coverage{false};

    bool blend_enabled{false};
    BackendBlendFactor source_blend{BackendBlendFactor::One};
    BackendBlendFactor destination_blend{BackendBlendFactor::Zero};
    BackendBlendOperation blend_operation{BackendBlendOperation::Add};
    BackendBlendFactor source_blend_alpha{BackendBlendFactor::One};
    BackendBlendFactor destination_blend_alpha{BackendBlendFactor::Zero};
    BackendBlendOperation blend_operation_alpha{BackendBlendOperation::Add};
    /// D3D11 IndependentBlendEnable. When false, the scalar RT0 state above
    /// applies to every active attachment, preserving legacy D3D9 callers.
    bool independent_blend_enabled{false};
    std::array<bool, BACKEND_MAX_RENDER_TARGETS> blend_enabled_targets{};
    std::array<BackendBlendFactor, BACKEND_MAX_RENDER_TARGETS> source_blends{};
    std::array<BackendBlendFactor, BACKEND_MAX_RENDER_TARGETS>
        destination_blends{};
    std::array<BackendBlendOperation, BACKEND_MAX_RENDER_TARGETS>
        blend_operations{};
    std::array<BackendBlendFactor, BACKEND_MAX_RENDER_TARGETS>
        source_blends_alpha{};
    std::array<BackendBlendFactor, BACKEND_MAX_RENDER_TARGETS>
        destination_blends_alpha{};
    std::array<BackendBlendOperation, BACKEND_MAX_RENDER_TARGETS>
        blend_operations_alpha{};
    /// Bit 0 red, 1 green, 2 blue, 3 alpha.
    unsigned color_write_mask{0xF};
    /// Per-MRT write masks. Slot 0 mirrors `color_write_mask` for existing
    /// single-target frontends.
    std::array<unsigned, BACKEND_MAX_RENDER_TARGETS> color_write_masks{
        0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF};

    /// Frontend-computed hash of everything above, so repeated frames reuse a
    /// compiled pipeline instead of recompiling shaders every draw.
    u64 cache_key{0};

    /// Raster sample count. Must match the color attachment (`1` = no MSAA).
    unsigned sample_count{1};

    /// Class of the primitives drawn with this pipeline. A caller that sets
    /// it MUST vary `cache_key` with it.
    BackendPrimitiveClass input_primitive_class{
        BackendPrimitiveClass::Unspecified};

    /// A tessellation pipeline (11.17): the vertex function is a
    /// post-tessellation one (`[[patch(...)]]`), fed by patch draws whose
    /// factors are half-precision MTL{Triangle,Quad}TessellationFactorsHalf
    /// records, one per patch. Vary `cache_key` with all of these.
    bool tessellation{false};
    BackendTessellationPartition tessellation_partition{
        BackendTessellationPartition::Integer};
    /// Output triangle winding: D3D triangle_cw / triangle_ccw.
    bool tessellation_clockwise{true};
    unsigned tessellation_max_factor{64};
};

class BackendPipeline {
public:
    virtual ~BackendPipeline() = default;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
};

enum class BackendTextureViewDimension : u8 {
    Automatic,
    Texture2D,
    Texture2DArray,
    Texture2DMS,
    Texture2DMSArray,
    TextureCube,
    TextureCubeArray,
};

/// Component mapping a shader view applies on top of its storage format.
enum class BackendViewSwizzle : u8 {
    Identity,
    /// (R, G, B, 1): storage whose fourth component is not data (B8G8R8X8).
    RGB1,
    /// (G, B, A, R): storage with the component order reversed per nibble
    /// (DXGI B4G4R4A4 held as Metal ABGR4Unorm).
    GBAR,
    /// (R, 0, 0, 1): a depth plane, whose missing components read 0 and
    /// alpha 1 (D3D11.3 functional spec §19.1).
    R001,
};

/// Shader-visible subresource window. Zero counts mean the backend's ordinary
/// full-resource/default-mip behavior; D3D11 supplies exact nonzero counts.
struct BackendTextureViewDesc {
    BackendTextureViewDimension dimension{BackendTextureViewDimension::Automatic};
    /// Next to `dimension`, in what was its padding: a view descriptor is
    /// held per slot in every BackendDrawCall, which has a size ratchet.
    BackendViewSwizzle swizzle{BackendViewSwizzle::Identity};
    /// A per-view component mapping applied after `swizzle`, in the D3D12
    /// Shader4ComponentMapping encoding (d3d12.h
    /// D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING): 3 bits per output component
    /// naming its source (0-3 = R,G,B,A, 4 = 0, 5 = 1), bit 12 set. 0 is the
    /// identity. Also in the padding before `pixel_format`.
    u16 component_mapping{0};
    /// Optional typed reinterpretation. Unknown preserves the parent format.
    PixelFormat pixel_format{PixelFormat::Unknown};
    unsigned most_detailed_mip{0};
    unsigned mip_levels{0};
    unsigned first_array_slice{0};
    unsigned array_size{0};
};

struct BackendComputePipelineDesc {
    std::string compute_shader_msl;
    std::string compute_entry_point{"compute_main"};
    BackendMathMode math_mode{BackendMathMode::Fast};
    /// Frontend-computed hash of everything above.
    u64 cache_key{0};
};

class BackendComputePipeline {
public:
    virtual ~BackendComputePipeline() = default;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
};

} // namespace vitreo::graphics
