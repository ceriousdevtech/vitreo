// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_draw_call.hpp
/// @brief Indirect arguments, dispatch calls, prebuilt native states, and draw calls.

#include "graphics/contracts/backend_bindings.hpp"
#include <array>

namespace vitreo::graphics {

/// A GPU-written argument block for an indirect draw or dispatch.
///
/// D3D and Metal spell these blocks identically — same members, same order,
/// same signedness — so the guest's buffer is passed through rather than read
/// back and repacked. Reading the counts on the CPU would defeat the API: the
/// point is that the GPU produced them and the CPU never sees them.
///
///   dispatch            3 x u32  {ThreadGroupCountX, Y, Z}
///   draw                4 x u32  {VertexCountPerInstance, InstanceCount,
///                                 StartVertexLocation, StartInstanceLocation}
///   indexed draw        5 x u32  {IndexCountPerInstance, InstanceCount,
///                                 StartIndexLocation, BaseVertexLocation (i32),
///                                 StartInstanceLocation}
///
/// A null buffer means the call is direct and the counts in the call struct are
/// authoritative. `offset` is a DWORD-aligned byte offset, as
/// D3D11 AlignedByteOffsetForArgs requires.
struct BackendIndirectArgs {
    const BackendBuffer* buffer{nullptr};
    unsigned offset{0};

    [[nodiscard]] bool active() const noexcept { return buffer != nullptr; }
};

struct BackendDispatchCall {
    const BackendComputePipeline* pipeline{nullptr};
    /// Threadgroup count (grid size).
    unsigned group_count_x{0};
    unsigned group_count_y{0};
    unsigned group_count_z{0};
    /// Threads per threadgroup (local work size).
    unsigned threads_per_group_x{1};
    unsigned threads_per_group_y{1};
    unsigned threads_per_group_z{1};

    /// Buffers bound at compute buffer indices.
    std::array<const BackendBuffer*, BACKEND_MAX_COMPUTE_BUFFERS> buffers{};
    std::array<unsigned, BACKEND_MAX_COMPUTE_BUFFERS> buffer_offsets{};

    /// Textures bound at compute texture indices.
    std::array<const BackendTexture2D*, BACKEND_MAX_COMPUTE_TEXTURES> textures{};
    std::array<BackendTextureViewDesc, BACKEND_MAX_COMPUTE_TEXTURES>
        texture_views{};
    /// Volume textures bound at compute texture indices (an RWTexture3D UAV
    /// or a Texture3D SRV). A slot names a 2D texture or a volume, never both.
    std::array<const BackendTexture3D*, BACKEND_MAX_COMPUTE_TEXTURES>
        textures_3d{};
    /// Typed buffers bound at compute texture indices: a `Buffer<T>` SRV at
    /// t#, an `RWBuffer<T>` UAV at BACKEND_UAV_TEXTURE_BASE + u#. The shader
    /// reads them as `texture_buffer`, so they share the texture namespace; a
    /// slot names exactly one of textures / textures_3d / texture_buffers.
    std::array<const BackendTextureBuffer*, BACKEND_MAX_COMPUTE_TEXTURES>
        texture_buffers{};
    /// Compute sampler s# bindings. Only the SRV half [0,7] consumes these;
    /// UAVs occupy the disjoint texture range [8,15].
    std::array<BackendSamplerDesc, 8> samplers{};
    std::array<bool, 8> sampler_bound{};
    /// s# (bit n) whose border twin binds at
    /// BACKEND_BORDER_TWIN_SAMPLER_BASE + n: `samplers[n]` with an
    /// opaque-white border. `samplers[n]` itself carries transparent black.
    u32 border_twin_mask{0};
    /// Inline constants at BACKEND_INLINE_CONSTANT_BUFFER_INDEX: the shader's
    /// push-constant block (sampler LOD biases and border colours).
    const void* constants{nullptr};
    usize constants_size{0};

    /// Descriptor tables, indexed by descriptor set. When any is non-null the
    /// backend binds tables and ignores the flat buffers/textures/samplers
    /// above, per the no-mixing rule.
    std::array<const BackendDescriptorTable*, BACKEND_MAX_DESCRIPTOR_SETS>
        descriptor_tables{};

    /// When active, the threadgroup counts come from this buffer and
    /// group_count_x/y/z are ignored. threads_per_group_* stay CPU-side in both
    /// APIs: D3D takes them from the shader's numthreads declaration and Metal
    /// from the encoder argument, so only the counts are GPU-sourced.
    BackendIndirectArgs indirect{};
};

/// native-state (delta_record.hpp, R3c): the states a draw names instead of
/// having the backend look its descs up. Every entry is optional.
struct BackendNativeStates {
    /// Built from exactly samplers[slot] / vertex_samplers[slot]
    /// (same_sampler_desc): the backend binds its native sampler and border
    /// twin. The descs stay filled either way (a texture view reads
    /// mipmaps_enabled).
    std::array<const BackendSamplerState*, BACKEND_MAX_TEXTURE_SLOTS>
        samplers{};
    std::array<const BackendSamplerState*, BACKEND_MAX_TEXTURE_SLOTS>
        vertex_samplers{};
    /// Built from exactly the call's depth/stencil fields
    /// (depth_stencil_desc_of), after read-only view masking and every
    /// override.
    const BackendDepthStencilState* depth_stencil{nullptr};
};

struct BackendDrawCall {
    const BackendPipeline* pipeline{nullptr};
    BackendPrimitiveTopology topology{BackendPrimitiveTopology::TriangleList};

    /// Slot 0 convenience alias; prefer `vertex_buffers[0]`.
    const BackendBuffer* vertex_buffer{nullptr};
    unsigned vertex_buffer_offset{0};

    /// D3D input slots → Metal vertex buffers (backend adds a base index).
    std::array<const BackendBuffer*, BACKEND_MAX_VERTEX_BUFFERS>
        vertex_buffers{};
    std::array<unsigned, BACKEND_MAX_VERTEX_BUFFERS> vertex_buffer_offsets{};

    /// The capture draw's output buffer, bound to the vertex stage at
    /// BACKEND_VERTEX_CAPTURE_BUFFER_INDEX. Null for every ordinary draw; only a
    /// pipeline built from a capture vertex shader writes it.
    const BackendBuffer* vertex_capture_buffer{nullptr};
    unsigned vertex_capture_buffer_offset{0};

    /// Null for a non-indexed draw.
    const BackendBuffer* index_buffer{nullptr};
    bool index_32bit{false};
    unsigned index_buffer_offset{0};

    unsigned first_vertex{0};
    /// Vertices for a non-indexed draw, indices for an indexed one.
    unsigned vertex_count{0};
    unsigned index_count{0};
    int base_vertex{0};
    /// Instance range for DrawInstanced/DrawIndexedInstanced. Ordinary Draw
    /// calls use one instance beginning at zero.
    unsigned instance_count{1};
    unsigned first_instance{0};

    /// Unordered-access views on the fragment stage's u# registers, bound by
    /// D3D11's OMSetRenderTargetsAndUnorderedAccessViews.
    ///
    /// Backends bind these at the same windows the compute stage uses: a texture
    /// UAV at texture index BACKEND_UAV_TEXTURE_BASE + slot, a buffer UAV at
    /// buffer index BACKEND_UAV_BUFFER_BASE + slot. A u# slot names a 2D texture,
    /// a volume, or a buffer -- never more than one.
    ///
    /// Only the fragment stage: D3D11.0 exposes u# to the pixel and compute
    /// stages alone (D3D11_PS_CS_UAV_REGISTER_COUNT), and compute has its own
    /// call struct. D3D11.1 widens this to every stage, which is why the u#
    /// window is kept disjoint from the input-assembler streams.
    std::array<const BackendTexture2D*, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_uav_textures{};
    std::array<BackendTextureViewDesc, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_uav_texture_views{};
    std::array<const BackendTexture3D*, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_uav_textures_3d{};
    std::array<const BackendBuffer*, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_uav_buffers{};
    std::array<unsigned, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_uav_buffer_offsets{};
    /// A typed buffer UAV (`RWBuffer<T>`) at texture index
    /// BACKEND_UAV_TEXTURE_BASE + slot: the shader addresses it as a
    /// read_write `texture_buffer`, so it takes the texture window, not the
    /// buffer one.
    std::array<const BackendTextureBuffer*, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_uav_texture_buffers{};
    /// The pixel shader's UAV counters, bound at
    /// BACKEND_UAV_COUNTER_BUFFER_BASE + k in the shader's own order. Each is
    /// the whole 4-byte counter buffer.
    std::array<const BackendBuffer*, BACKEND_MAX_UAV_COUNTER_BINDINGS>
        fragment_uav_counters{};
    /// The PIPELINE_STATISTICS record a counting pixel shader adds to, bound
    /// at BACKEND_FRAGMENT_STATISTICS_BUFFER_INDEX. Null for every other draw.
    const BackendBuffer* fragment_statistics_buffer{nullptr};

    /// Descriptor tables per stage, indexed by descriptor set. When any is
    /// non-null the backend binds tables for that stage and ignores its flat
    /// constant buffers, textures, samplers and SRV buffers.
    std::array<const BackendDescriptorTable*, BACKEND_MAX_DESCRIPTOR_SETS>
        vertex_descriptor_tables{};
    std::array<const BackendDescriptorTable*, BACKEND_MAX_DESCRIPTOR_SETS>
        fragment_descriptor_tables{};

    /// When active, every count above (vertex_count / index_count /
    /// instance_count / first_vertex / first_instance / base_vertex) comes from
    /// this buffer instead. Whether the draw is indexed is still decided by
    /// index_buffer, because that selects which argument layout the buffer holds.
    BackendIndirectArgs indirect{};

    /// Uniform bytes bound at buffer index 0 of each stage. Small enough to
    /// go inline; a backend must not require the caller to own a GPU buffer
    /// for per-draw constants.
    ///
    /// These point at caller memory that is only guaranteed to live for the
    /// duration of the `draw()` call, so a backend must copy what it needs
    /// before returning rather than retaining the pointer.
    const void* vertex_constants{nullptr};
    usize vertex_constants_size{0};
    const void* fragment_constants{nullptr};
    usize fragment_constants_size{0};

    /// Constant buffers bound at buffer indices 1..N. D3D11 and Vulkan use
    /// explicit buffer slots; set*Bytes covers slot 0, these cover 1+.
    /// The offset is added to the buffer base before binding.
    std::array<const BackendBuffer*, BACKEND_MAX_CONSTANT_BUFFER_SLOTS>
        vertex_constant_buffers{};
    std::array<unsigned, BACKEND_MAX_CONSTANT_BUFFER_SLOTS>
        vertex_constant_buffer_offsets{};
    std::array<const BackendBuffer*, BACKEND_MAX_CONSTANT_BUFFER_SLOTS>
        fragment_constant_buffers{};
    std::array<unsigned, BACKEND_MAX_CONSTANT_BUFFER_SLOTS>
        fragment_constant_buffer_offsets{};

    /// Per-slot 2D/cube sample source. Mutually exclusive with `textures_3d`
    /// for a given slot; when both are non-null, backends bind `textures`.
    std::array<const BackendTexture2D*, BACKEND_MAX_TEXTURE_SLOTS> textures{};
    std::array<BackendTextureViewDesc, BACKEND_MAX_TEXTURE_SLOTS>
        texture_views{};
    /// Per-slot 3D/volume sample source (D3D9 IDirect3DVolumeTexture9).
    std::array<const BackendTexture3D*, BACKEND_MAX_TEXTURE_SLOTS> textures_3d{};
    std::array<BackendSamplerDesc, BACKEND_MAX_TEXTURE_SLOTS> samplers{};
    /// Vertex-stage texture namespace. D3D9 vs_3_0 exposes four independent
    /// samplers; the shared backend keeps the full slot count symmetric.
    std::array<const BackendTexture2D*, BACKEND_MAX_TEXTURE_SLOTS>
        vertex_textures{};
    std::array<BackendTextureViewDesc, BACKEND_MAX_TEXTURE_SLOTS>
        vertex_texture_views{};
    std::array<const BackendTexture3D*, BACKEND_MAX_TEXTURE_SLOTS>
        vertex_textures_3d{};
    std::array<BackendSamplerDesc, BACKEND_MAX_TEXTURE_SLOTS>
        vertex_samplers{};
    /// s# (bit n) whose border twin binds at
    /// BACKEND_BORDER_TWIN_SAMPLER_BASE + n: the stage's sampler n with an
    /// opaque-white border, where sampler n carries transparent black. A mask
    /// rather than eight more descriptors, because this struct is built per
    /// draw and a twin differs from its sampler only in the border.
    u32 fragment_border_twin_mask{0};
    u32 vertex_border_twin_mask{0};

    /// StructuredBuffer SRVs bound to t# registers. A t# slot names either a
    /// texture or a buffer, never both, so these share the texture slot
    /// numbering. Backends bind them at buffer index
    /// BACKEND_SRV_BUFFER_BASE + slot, which is the index the shader
    /// translator assigns; the offset is added to the buffer base.
    std::array<const BackendBuffer*, BACKEND_MAX_TEXTURE_SLOTS>
        vertex_srv_buffers{};
    std::array<unsigned, BACKEND_MAX_TEXTURE_SLOTS>
        vertex_srv_buffer_offsets{};
    std::array<const BackendBuffer*, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_srv_buffers{};
    std::array<unsigned, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_srv_buffer_offsets{};

    /// Typed buffer SRVs bound to t# registers. These are textures, not
    /// buffers: backends bind them at texture index `slot`, the same index a
    /// Texture2D in that slot would take, because a t# register names one
    /// resource. A slot is a texture, a structured buffer, or one of these,
    /// never more than one.
    std::array<const BackendTextureBuffer*, BACKEND_MAX_TEXTURE_SLOTS>
        vertex_srv_texture_buffers{};
    std::array<const BackendTextureBuffer*, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_srv_texture_buffers{};

    /// x, y, width, height, near, far.
    std::array<float, 6> viewport{};
    bool scissor_enabled{false};
    /// left, top, right, bottom.
    std::array<int, 4> scissor{};
    /// Viewport ARRAY, for shaders that write D3D's SV_ViewportArrayIndex.
    /// Null keeps the single `viewport`/`scissor` above. Non-null:
    /// `viewport_count` entries of each (caller-owned, read during the call)
    /// replace them, and every slot past the count is DISABLED -- a primitive
    /// sent there is not drawn (Learn RSSetViewports; measured on Windows,
    /// oracle scene vs_viewport_index). `scissor_enabled` still selects
    /// between `scissors` and each viewport clipping to itself.
    const std::array<float, 6>* viewports{nullptr};
    const std::array<int, 4>* scissors{nullptr};
    unsigned viewport_count{0};

    /// A patch draw through a tessellation pipeline (11.17): `patch_count`
    /// patches of `patch_control_points` control points, factors read from
    /// `tessellation_factors` (one record per patch). The vertex counts and
    /// buffers above are unused.
    const BackendBuffer* tessellation_factors{nullptr};
    unsigned patch_count{0};
    unsigned patch_control_points{0};

    BackendCullMode cull_mode{BackendCullMode::None};
    BackendFillMode fill_mode{BackendFillMode::Fill};
    /// Clip fragments outside D3D's 0 <= z <= w range. False requests depth
    /// clamping while preserving the mandatory w > 0 clip.
    bool depth_clip_enabled{true};
    float depth_bias{0.0f};
    float depth_bias_slope_scale{0.0f};
    float depth_bias_clamp{0.0f};
    /// Dynamic RGBA blend constant. Used when the pipeline selects
    /// BlendColor or OneMinusBlendColor.
    std::array<float, 4> blend_color{1.0f, 1.0f, 1.0f, 1.0f};
    /// True when counter-clockwise triangles are front-facing. D3D11 exposes
    /// this directly as D3D11_RASTERIZER_DESC::FrontCounterClockwise.
    bool front_counter_clockwise{false};
    bool depth_test_enabled{false};
    bool depth_write_enabled{false};
    BackendCompare depth_compare{BackendCompare::LessEqual};
    bool stencil_test_enabled{false};
    u8 stencil_read_mask{0xFF};
    u8 stencil_write_mask{0xFF};
    u32 stencil_reference{0};
    BackendStencilFaceDesc front_stencil{};
    BackendStencilFaceDesc back_stencil{};
    /// native-state (R3c), optional: prebuilt states this call names
    /// (BackendNativeStates), owned by the caller for the call's duration.
    /// One pointer, so the per-draw struct stays within its size ratchet.
    const BackendNativeStates* native_states{nullptr};
};

/// The depth/stencil fields of @p call as one desc.
[[nodiscard]] inline BackendDepthStencilDesc depth_stencil_desc_of(
    const BackendDrawCall& call) noexcept {
    BackendDepthStencilDesc d{};
    d.depth_test_enabled = call.depth_test_enabled;
    d.depth_write_enabled = call.depth_write_enabled;
    d.depth_compare = call.depth_compare;
    d.stencil_test_enabled = call.stencil_test_enabled;
    d.stencil_read_mask = call.stencil_read_mask;
    d.stencil_write_mask = call.stencil_write_mask;
    d.front_stencil = call.front_stencil;
    d.back_stencil = call.back_stencil;
    return d;
}

/// Write @p desc into @p call's depth/stencil fields.
inline void apply_depth_stencil_desc(BackendDrawCall& call,
                                     const BackendDepthStencilDesc& desc) noexcept {
    call.depth_test_enabled = desc.depth_test_enabled;
    call.depth_write_enabled = desc.depth_write_enabled;
    call.depth_compare = desc.depth_compare;
    call.stencil_test_enabled = desc.stencil_test_enabled;
    call.stencil_read_mask = desc.stencil_read_mask;
    call.stencil_write_mask = desc.stencil_write_mask;
    call.front_stencil = desc.front_stencil;
    call.back_stencil = desc.back_stencil;
}

} // namespace vitreo::graphics
