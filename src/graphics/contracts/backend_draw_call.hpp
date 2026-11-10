// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_draw_call.hpp
/// @brief Indirect arguments, dispatch calls, prebuilt native states, and draw calls.

#include "graphics/contracts/backend_bindings.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

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
/// authoritative. `offset` is a DWORD-aligned byte offset.
/// D3D12 ExecuteIndirect's ArgumentBufferOffset is a UINT64
/// (D3D12_GPU_VIRTUAL_ADDRESS). D3D11's AlignedByteOffsetForArgs fits in
/// 32 bits and is stored in the same field.
struct BackendIndirectArgs {
    const BackendBuffer* buffer{nullptr};
    u64 offset{0};

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
    std::array<u64, BACKEND_MAX_COMPUTE_BUFFERS> buffer_offsets{};

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

// ---- WS5 begin ----
/// A draw through an object+mesh pipeline (BackendPipeline::is_mesh): the
/// object stage launches `mesh_groups` mesh threadgroups in chunks of at most
/// 1024 per object threadgroup (WWDC22 10162; Feature Set Tables).
struct BackendMeshDraw {
    /// ceil(mesh_groups / 1024); 0 = not a mesh draw.
    unsigned object_threadgroups{0};
    /// Total mesh threadgroups, placed in object-stage buffer 0.
    unsigned mesh_groups{0};
    /// Bound on the mesh stage at BACKEND_GS_EXPANSION_BUFFER_BASE + i.
    std::array<const BackendBuffer*, 4> expansion_buffers{};
    std::array<u64, 4> expansion_offsets{};
    /// D3D12 DispatchMesh (graphics/shaders/mesh_stage.hpp), with
    /// object_threadgroups 0: `grid` threadgroups of the object function
    /// (the amplification shader) or, without one, of the mesh function,
    /// launched as given; or, with `indirect`, the three UINTs at
    /// indirect_offset (D3D12_DISPATCH_MESH_ARGUMENTS, the layout of
    /// MTLDispatchThreadgroupsIndirectArguments). The threadgroup shapes
    /// are the shaders' numthreads. The call's vertex-stage tables and push
    /// block go to the mesh stage, its object_* ones to the object stage.
    std::array<unsigned, 3> grid{};
    std::array<unsigned, 3> object_threads{1, 1, 1};
    std::array<unsigned, 3> mesh_threads{1, 1, 1};
    const BackendBuffer* indirect{nullptr};
    u64 indirect_offset{0};
    /// A PIPELINE_STATISTICS variant's record (mesh_stage.hpp
    /// kMeshStatWords), bound on the object and mesh stages at
    /// kMeshStatisticsBufferIndex.
    const BackendBuffer* statistics{nullptr};
    /// Issue memoryBarrier(buffers, after: vertex, before: object|mesh)
    /// before this draw, so a mesh stage reads what an earlier vertex stage
    /// of the same encoder wrote (Feature Set Tables fn 3: Apple3-10).
    bool barrier_before_mesh{false};
    /// A D3D12 mesh draw's amplification shader: its tables and push block.
    std::array<const BackendDescriptorTable*, BACKEND_MAX_DESCRIPTOR_SETS>
        object_descriptor_tables{};
    const void* object_constants{nullptr};
    usize object_constants_size{0};
    /// A device-staging mesh pipeline (mesh_stage.hpp
    /// MeshStageLayout::device_staging): its scratch, bound on the mesh
    /// stage at kMeshScratchBufferIndex; `group_origin` (a uint4 at
    /// kMeshOriginBufferIndex of both stages) is added to SV_GroupID of the
    /// chunk `grid` is; `serialize` orders this draw's object and mesh
    /// stages after the previous draw's mesh stage (the chunks share the
    /// scratch).
    const BackendBuffer* scratch{nullptr};
    std::array<unsigned, 4> group_origin{};
    bool serialize{false};
    /// When set, the uint4 at kMeshOriginBufferIndex is read from this
    /// buffer at `origin_offset` instead of `group_origin` (a chunk of an
    /// indirect dispatch whose origin the GPU resolved).
    const BackendBuffer* origin_buffer{nullptr};
    u64 origin_offset{0};
    /// A raster-capture mesh pipeline (MeshStageLayout::raster_capture):
    /// the record buffer, bound on the mesh stage at kMeshCaptureBufferIndex.
    const BackendBuffer* capture{nullptr};
    /// D3D12 view instancing (mesh_stage.hpp vi_capture): the capture
    /// records bound at kMeshViewCaptureBufferIndex on the object stage and
    /// on the mesh stage.
    const BackendBuffer* object_capture{nullptr};
    const BackendBuffer* mesh_capture{nullptr};
    [[nodiscard]] bool d3d12_dispatch() const {
        return object_threadgroups == 0 && (grid[0] != 0 || indirect != nullptr);
    }
    [[nodiscard]] bool active() const { return object_threadgroups != 0 || d3d12_dispatch(); }
};
// ---- WS5 end ----

struct BackendDrawCall {
    /// Explicit frontend certificate: every indirectly reachable resource is
    /// read-only in the FINAL executable stages. Unknown/native MSL is false.
    /// Render/depth attachments remain governed by their separate bindings.
    bool indirect_resources_read_only{false};
    const BackendPipeline* pipeline{nullptr};
    // ---- WS5 begin ----
    /// With a mesh pipeline: the mesh dispatch. Every binding the call puts
    /// in a vertex-stage field (constant buffers, textures, samplers, SRV
    /// buffers, the push block at 0) is bound on the MESH stage instead;
    /// fragment bindings are unchanged. Out of line (null for every other
    /// draw): the call is built per draw and few draws are mesh draws
    /// (BackendBindingLayout.CallStructsStayWithinTheirSizeRatchet). The
    /// backend reads it during BackendRenderPass::draw only.
    const BackendMeshDraw* mesh{nullptr};
    [[nodiscard]] bool mesh_active() const {
        return mesh != nullptr && mesh->active();
    }
    /// The object stage's tables (none without a mesh draw).
    [[nodiscard]] const std::array<const BackendDescriptorTable*,
                                   BACKEND_MAX_DESCRIPTOR_SETS>&
    object_descriptor_tables() const {
        static constexpr std::array<const BackendDescriptorTable*,
                                    BACKEND_MAX_DESCRIPTOR_SETS>
            kNone{};
        return mesh != nullptr ? mesh->object_descriptor_tables : kNone;
    }
    // ---- WS5 end ----
    BackendPrimitiveTopology topology{BackendPrimitiveTopology::TriangleList};

    /// Slot 0 convenience alias; prefer `vertex_buffers[0]`.
    const BackendBuffer* vertex_buffer{nullptr};
    u64 vertex_buffer_offset{0};

    /// One entry per Windows input slot. Stream s binds at
    /// vertex_stream_buffer_index(s) when vertex_stream_bindable(s).
    std::array<const BackendBuffer*, BACKEND_MAX_VERTEX_BUFFERS>
        vertex_buffers{};
    std::array<u64, BACKEND_MAX_VERTEX_BUFFERS> vertex_buffer_offsets{};
    /// Each stream's stride, read only when `pipeline` was built with
    /// BackendPipelineDesc::dynamic_vertex_strides (non-zero there).
    std::array<u32, BACKEND_MAX_VERTEX_BUFFERS> vertex_buffer_strides{};

    /// Vertex pulling (graphics::DxbcTranslationOptions::vertex_pulling):
    /// the pull table, bound to the vertex stage at
    /// BACKEND_VERTEX_BUFFER_BASE. With it set, `vertex_buffers` are not
    /// bound as streams: they are the buffers the table's addresses point
    /// into, and are only made resident (read) for the draw.
    const BackendBuffer* vertex_pull_table{nullptr};
    u64 vertex_pull_table_offset{0};

    /// The capture draw's output buffer, bound to the vertex stage at
    /// BACKEND_VERTEX_CAPTURE_BUFFER_INDEX. Null for every ordinary draw; only a
    /// pipeline built from a capture vertex shader writes it.
    const BackendBuffer* vertex_capture_buffer{nullptr};
    u64 vertex_capture_buffer_offset{0};

    /// Null for a non-indexed draw.
    const BackendBuffer* index_buffer{nullptr};
    bool index_32bit{false};
    u64 index_buffer_offset{0};

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
    std::array<u64, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_uav_buffer_offsets{};
    /// A typed buffer UAV (`RWBuffer<T>`) at texture index
    /// BACKEND_UAV_TEXTURE_BASE + slot: the shader addresses it as a
    /// read_write `texture_buffer`, so it takes the texture window, not the
    /// buffer one.
    std::array<const BackendTextureBuffer*, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_uav_texture_buffers{};
    /// The pixel shader's UAV counters, bound at
    /// BACKEND_UAV_COUNTER_BUFFER_BASE + k in the shader's own order.
    std::array<const BackendBuffer*, BACKEND_MAX_UAV_COUNTER_BINDINGS>
        fragment_uav_counters{};
    /// Byte offset of counter k in that buffer: the descriptor's
    /// CounterOffsetInBytes plus the counter resource's placement. Zero
    /// addresses the first byte, which is the whole-buffer bind.
    std::array<u64, BACKEND_MAX_UAV_COUNTER_BINDINGS>
        fragment_uav_counter_offsets{};
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
    /// The byte offset is a D3D12_GPU_VIRTUAL_ADDRESS difference (UINT64) and is
/// added to the buffer base before binding. It is not truncated to 32 bits.
    std::array<const BackendBuffer*, BACKEND_MAX_CONSTANT_BUFFER_SLOTS>
        vertex_constant_buffers{};
    std::array<u64, BACKEND_MAX_CONSTANT_BUFFER_SLOTS>
        vertex_constant_buffer_offsets{};
    std::array<const BackendBuffer*, BACKEND_MAX_CONSTANT_BUFFER_SLOTS>
        fragment_constant_buffers{};
    std::array<u64, BACKEND_MAX_CONSTANT_BUFFER_SLOTS>
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
    std::array<u64, BACKEND_MAX_TEXTURE_SLOTS>
        vertex_srv_buffer_offsets{};
    std::array<const BackendBuffer*, BACKEND_MAX_TEXTURE_SLOTS>
        fragment_srv_buffers{};
    std::array<u64, BACKEND_MAX_TEXTURE_SLOTS>
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
    /// False when no pixel shader runs or every render-target write mask
    /// is zero. Default true: a draw can change colour attachments.
    bool writes_color{true};
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

/// The planes of the depth-stencil attachment @p call can change.
///
/// Depth: depth writes are on. Stencil -- D3D11.3 17.9 and
/// D3D11_DEPTH_STENCILOP_DESC: the stencil buffer changes only through
/// StencilFailOp, StencilDepthFailOp and StencilPassOp, under
/// StencilWriteMask, while the stencil test is enabled. A test whose
/// operations are all Keep on both faces, or whose write mask is zero,
/// writes nothing.
struct DepthStencilWrites {
    bool depth{false};
    bool stencil{false};
};
[[nodiscard]] inline DepthStencilWrites depth_stencil_writes_of(
    const BackendDrawCall& call) noexcept {
    const auto face_writes = [](const BackendStencilFaceDesc& face) {
        return face.stencil_fail != BackendStencilOperation::Keep ||
               face.depth_fail != BackendStencilOperation::Keep ||
               face.pass != BackendStencilOperation::Keep;
    };
    return DepthStencilWrites{
        call.depth_write_enabled,
        call.stencil_test_enabled && call.stencil_write_mask != 0 &&
            (face_writes(call.front_stencil) || face_writes(call.back_stencil)),
    };
}

/// How a buffer byte offset was combined with the buffer's native placement.
/// Learn: D3D12_GPU_VIRTUAL_ADDRESS is a UINT64. Metal's setBuffer:offset:
/// is pointer-width (NSUInteger): on arm64 that holds every 64-bit byte index.
enum class BufferByteOffset : u8 {
    /// @p out is view + native, and it fits the backend offset.
    Bound,
    /// The backend offset is narrower than this byte index. The slot stays
    /// empty; the offset is not truncated onto a low address.
    Unheld,
    /// view + native does not fit in 64 bits. It is not wrapped, and it is
    /// not reported as offset 0.
    Overflow,
};

/// One `[vitreo] UNIMPLEMENTED d3d12!ExecuteCommandLists: ...` line per case.
inline void log_buffer_offset_gap(const char* what) noexcept {
    static std::mutex mutex;
    static const char* seen[4]{};
    static unsigned count = 0;
    std::lock_guard<std::mutex> lock(mutex);
    for (unsigned i = 0; i < count; ++i) {
        if (std::strcmp(seen[i], what) == 0) return;
    }
    if (count < 4) seen[count++] = what;
    std::fprintf(stderr,
                 "[vitreo] UNIMPLEMENTED d3d12!ExecuteCommandLists: %s\n",
                 what);
}

/// Fold @p view (a byte offset into the buffer) and @p native (the buffer's
/// own placement in its storage) into the offset Metal passes to
/// setBuffer:offset:. A sum that does not fit in 64 bits, and a view the
/// pointer-width offset cannot name, leave @p out unchanged.
[[nodiscard]] inline BufferByteOffset buffer_byte_offset(u64 view, u64 native,
                                                         u64& out) noexcept {
    if constexpr (sizeof(usize) < sizeof(u64)) {
        if (view > static_cast<u64>(~usize{0})) {
            log_buffer_offset_gap(
                "a buffer view more than 4 GiB into its heap");
            return BufferByteOffset::Unheld;
        }
    }
    if (native > ~u64{0} - view) {
        log_buffer_offset_gap(
            "a buffer view whose byte offset does not fit in 64 bits");
        return BufferByteOffset::Overflow;
    }
    const u64 sum = native + view;
    if constexpr (sizeof(usize) < sizeof(u64)) {
        if (sum > static_cast<u64>(~usize{0})) {
            log_buffer_offset_gap(
                "a buffer view more than 4 GiB into its heap");
            return BufferByteOffset::Unheld;
        }
    }
    out = sum;
    return BufferByteOffset::Bound;
}

/// @p out is the pointer-width offset for setBuffer:offset:. False leaves
/// the slot empty: @p out is not set to 0 as a stand-in.
[[nodiscard]] inline bool metal_buffer_offset(u64 view, usize native,
                                              usize& out) noexcept {
    u64 sum = 0;
    if (buffer_byte_offset(view, static_cast<u64>(native), sum) !=
        BufferByteOffset::Bound) {
        return false;
    }
    out = static_cast<usize>(sum);
    return true;
}

} // namespace vitreo::graphics
