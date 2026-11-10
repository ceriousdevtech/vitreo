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
/// Input-assembler slots. mingw-w64 13.0.0 `d3d12.h` and `d3d11.h`:
/// `D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT` / 
/// `D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT` are `(32)`. A stream past
/// Metal's buffer table is not bindable (`vertex_stream_bindable`).
constexpr unsigned BACKEND_MAX_VERTEX_BUFFERS = 32;

/// Floating-point optimisation contract a translated shader is compiled
/// under. Fast is Metal's default (no INF/NaN, reassociation); Relaxed allows
/// reassociation but "preserves infs and nans"; Safe allows neither
/// (Metal SDK MTLLibrary.h, MTLMathMode). D3D11.3 3.1.3.1 requires INF/NaN
/// semantics for every shader and 22.3.2 forbids refactoring unless the
/// shader declares refactoringAllowed and the instruction is not precise.
enum class BackendMathMode : u8 { Fast, Relaxed, Safe };

struct BackendPipelineDesc {
    bool operator==(const BackendPipelineDesc&) const = default;
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
    /// Stride per input-assembler stream (index =
    /// BackendVertexAttribute::buffer_index). One entry per Windows slot.
    std::array<unsigned, BACKEND_MAX_VERTEX_BUFFERS> vertex_buffer_strides{};
    /// Per-input-slot fetch classification. False advances per vertex; true
    /// advances once per `vertex_buffer_step_rates[slot]` instances. A step
    /// rate of 0 never advances: every instance reads the element at
    /// StartInstanceLocation (D3D11.3 8.3.1 pseudocode; measured,
    /// tools/oracle_probe/d3d11_a07_ia_probe.expected `B step=0`).
    std::array<bool, BACKEND_MAX_VERTEX_BUFFERS> vertex_buffer_per_instance{};
    std::array<unsigned, BACKEND_MAX_VERTEX_BUFFERS> vertex_buffer_step_rates{};
    /// The guest bound this slot with stride 0: every vertex AND every
    /// instance fetches the element at the binding offset, whatever the
    /// slot's classification, step rate or StartInstanceLocation (measured,
    /// tools/oracle_probe/d3d11_instance_id_probe.expected). Overrides the
    /// slot's stride and step function.
    std::array<bool, BACKEND_MAX_VERTEX_BUFFERS> vertex_buffer_constant{};
    /// Legacy single-stream stride (slot 0). Prefer vertex_buffer_strides[0].
    unsigned vertex_stride{0};
    /// Every stream an attribute reads takes its stride from the draw
    /// (BackendDrawCall::vertex_buffer_strides) instead of the pipeline:
    /// Metal's MTLBufferLayoutStrideDynamic with setVertexBuffer:offset:
    /// attributeStride:atIndex: (macOS 14). vertex_buffer_strides and
    /// vertex_buffer_constant are ignored; a draw with a stride-0 stream
    /// needs a static pipeline. The pipeline is invalid where the OS has no
    /// dynamic strides (BackendPipeline::dynamic_vertex_strides then false).
    /// Vary `cache_key` with it.
    bool dynamic_vertex_strides{false};

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

    /// When false, a pipeline whose shaders or Metal state are not compiled
    /// yet is returned invalid immediately. The compile continues in the
    /// background and a later create of the same key succeeds. A draw must
    /// not wait on newLibraryWithSource or pipeline-state creation: that
    /// wait is a hitch on the first frames after a load. Tests and one-shot
    /// callers leave this true so create returns a finished pipeline.
    bool synchronous_compile{true};

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
    // ---- WS5 begin ----
    /// An object+mesh pipeline (GraphicsBackendDevice::create_mesh_pipeline):
    /// drawn only by a BackendDrawCall carrying a BackendMeshDraw.
    [[nodiscard]] virtual bool is_mesh() const noexcept { return false; }
    // ---- WS5 end ----
    /// Built with BackendPipelineDesc::dynamic_vertex_strides: its draws
    /// bind each stream with BackendDrawCall::vertex_buffer_strides.
    [[nodiscard]] virtual bool dynamic_vertex_strides() const noexcept { return false; }
};

// ---- WS5 begin ----
/// A render pipeline whose geometry comes from an object and a mesh function
/// instead of a vertex function (Metal 3, MTLMeshRenderPipelineDescriptor;
/// mesh shading needs MTLGPUFamilyApple7, Metal Feature Set Tables).
struct BackendMeshPipelineDesc {
    /// Render-target, blend, depth, sample, alpha-to-coverage, fragment and
    /// cache_key fields are used as for a vertex pipeline. vertex_shader_msl
    /// holds the ONE library with both the object and the mesh function
    /// (compiled with vertex_math_mode); vertex_attributes must be empty,
    /// tessellation false and rasterization_enabled true.
    BackendPipelineDesc render;
    /// Empty: no object function (a D3D12 mesh pipeline without an
    /// amplification shader).
    std::string object_entry_point{"vitreo_gs_object"};
    /// Non-empty: the object function's own library (compiled with
    /// object_math_mode); else it is in render.vertex_shader_msl.
    std::string object_shader_msl;
    BackendMathMode object_math_mode{BackendMathMode::Fast};
    std::string mesh_entry_point{"main0"};
    unsigned threads_per_object_threadgroup{1};
    unsigned threads_per_mesh_threadgroup{1};
    unsigned payload_bytes{16};
};
// ---- WS5 end ----

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

/// The backend's text for the last pipeline it failed to create on this
/// thread (the Metal compiler's or pipeline-state error), so a frontend can
/// name it in its refusal. A frontend clears it before a creation it wants
/// the reason of; a create_* that fails sets it.
[[nodiscard]] inline std::string& last_pipeline_failure() {
    thread_local std::string text;
    return text;
}

} // namespace vitreo::graphics
