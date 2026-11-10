// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <string>
#include "graphics/metal/metal_gpu_profile.hpp"
#include "graphics/metal/metal_gpu_watch.hpp"
#include <vitreo/common.hpp>

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vitreo::graphics {

class MetalResidentLog;    // metal_submit_queue.hpp
struct MetalResidentToken;

/// Records Metal commands into a command buffer.
/// All void* params are bridged Objective-C Metal objects.
class MetalCommandRecorder {
public:
    explicit MetalCommandRecorder(void* command_queue);  // id<MTLCommandQueue>
    ~MetalCommandRecorder();

    // -- Command buffer lifecycle ---
    void begin();
    /// Begin a command buffer only if one is not already open. Used by the
    /// device-level frame batcher so many render passes share one commit.
    void ensure_begun();
    [[nodiscard]] bool has_command_buffer() const noexcept {
        return m_command_buffer != nullptr;
    }
    void* command_buffer() const { return m_command_buffer; }
    /// The command buffer with no encoder open on it, for a caller that
    /// encodes encoders of its own (MetalSubmitQueue::record_encode).
    void* command_buffer_for_encoders() {
        end_active_encoder();
        return m_command_buffer;
    }
    /// Recycle bump-allocated VS/PS constant slabs after a presented frame
    /// is committed. Mid-frame commits (encoder throttle, readback) must not
    /// call this: in-flight command buffers still read the current slot.
    void recycle_constant_slabs_after_present();

    // -- Render pass ---
    void begin_render_pass(void* descriptor);   // MTLRenderPassDescriptor*
    void end_render_pass();
    /// Count the samples that pass depth and stencil into the open pass's
    /// visibilityResultBuffer at @p offset (MTLVisibilityResultModeCounting).
    void set_visibility_counting(usize offset);

    // -- Render state ---
    void set_render_pipeline(void* pso);        // id<MTLRenderPipelineState>
    void set_depth_stencil(void* state);        // id<MTLDepthStencilState>

    /// True while a render pass is open. Callers must not record draw state
    /// when this is false: the encoder has been ended and released.
    [[nodiscard]] bool has_render_encoder() const noexcept {
        return m_render_encoder != nullptr;
    }
    /// @p stride non-zero: the bound pipeline takes this stream's stride
    /// from the binding (BackendPipelineDesc::dynamic_vertex_strides).
    void set_vertex_buffer(void* buffer, usize offset, u32 index, u32 stride = 0);
    void set_fragment_buffer(void* buffer, usize offset, u32 index);
    /// Bind small per-draw constants inline, without a GPU buffer.
    void set_vertex_bytes(const void* data, usize length, u32 index);
    void set_fragment_bytes(const void* data, usize length, u32 index);
    void set_vertex_texture(void* texture, u32 index);
    void set_fragment_texture(void* texture, u32 index);
    void set_vertex_sampler(void* sampler, u32 index);
    void set_fragment_sampler(void* sampler, u32 index);

    // Viewport: {x, y, width, height, znear, zfar} as doubles
    void set_viewport(f64 x, f64 y, f64 w, f64 h, f64 znear, f64 zfar);
    void set_scissor(u32 x, u32 y, u32 w, u32 h);
    /// Whole viewport and scissor arrays (equal counts). Invalidates the
    /// single-viewport caches, since Metal's setViewport replaces the array.
    /// drawPatches over patches 0..count-1 with the factor buffer bound.
    void draw_patches(void* factors, usize factors_offset, u32 control_points,
                      u32 patch_count, u32 instance_count, u32 first_instance);
    void set_viewports(const std::array<float, 6>* viewports,
                       const std::array<u32, 4>* scissors, u32 count);
    void set_cull_mode(u32 mode);       // MTLCullMode
    void set_triangle_fill_mode(u32 mode); // MTLTriangleFillMode
    void set_depth_clip_mode(u32 mode); // MTLDepthClipMode
    void set_depth_bias(f32 bias, f32 slope_scale, f32 clamp);
    void set_front_facing(u32 winding); // MTLWinding
    void set_blend_color(f32 r, f32 g, f32 b, f32 a);
    void set_stencil_reference(u32 ref);

    // -- Draw calls ---
    void draw(u32 primitive_type, u32 vertex_count, u32 start);
    void draw_indexed(u32 primitive_type, u32 index_count,
                      u32 index_type, void* index_buffer, usize offset);
    void draw_instanced(u32 primitive_type, u32 vertex_count,
                        u32 instance_count, u32 start_vertex, u32 start_instance);
    void draw_indexed_instanced(u32 primitive_type, u32 index_count,
                                u32 index_type, void* index_buffer, usize offset,
                                u32 instance_count, i32 base_vertex,
                                u32 start_instance);
    /// GPU-sourced draw. `args_buffer` holds MTLDrawPrimitivesIndirectArguments
    /// at `args_offset`, which is byte-identical to the layout D3D's
    /// DrawInstancedIndirect reads, so the guest buffer passes straight through.
    void draw_instanced_indirect(u32 primitive_type, void* args_buffer,
                                 usize args_offset);
    /// GPU-sourced indexed draw. `args_buffer` holds
    /// MTLDrawIndexedPrimitivesIndirectArguments at `args_offset`.
    void draw_indexed_instanced_indirect(u32 primitive_type, u32 index_type,
                                         void* index_buffer, usize index_offset,
                                         void* args_buffer, usize args_offset);

    /// Mark resources reachable only through an argument buffer as resident.
    /// Metal makes a directly-bound resource resident implicitly; one reached
    /// through a descriptor table is not, and reading it without this either
    /// faults or returns nothing. @p resources may be null when @p count is 0.
    void use_render_resources(void* const* resources, u32 count,
                              bool read_only);
    void use_render_resources(const std::vector<void*>& resources,
                              bool read_only = false);

    // ---- WS5 begin ----
    /// Mesh-stage bindings (Metal keeps them apart from the vertex stage's,
    /// so a mesh draw neither reads nor disturbs the vertex binding caches).
    /// Uncached: each mesh draw binds what it reads.
    void set_mesh_buffer(void* buffer, usize offset, u32 index);
    void set_mesh_bytes(const void* data, usize length, u32 index);
    /// nullptr binds the black dummy, as set_vertex_texture does.
    void set_mesh_texture(void* texture, u32 index);
    void set_mesh_sampler(void* sampler, u32 index);
    void set_object_bytes(const void* data, usize length, u32 index);
    /// memoryBarrierWithScope:Buffers afterStages:Vertex
    /// beforeStages:Object|Mesh (Feature Set Tables fn 3: Apple3-10).
    void memory_barrier_vertex_to_mesh();
    /// Buffer writes of earlier object / mesh stages before this draw's
    /// object and mesh stages read them (device-staging chunks sharing a
    /// scratch; D3D12 view-instancing records).
    void memory_barrier_mesh_to_mesh();
    /// A fragment UAV or statistics write is visible to the next draw's
    /// vertex and fragment stages without ending the render pass. Ending
    /// the pass would load and store every attachment.
    ///
    /// The scope form waits for every texture, attachments included, so a
    /// tile that still holds the colour and depth targets has to store and
    /// reload them. Use it only when the written resources cannot be named.
    void memory_barrier_fragment_writes();
    /// The same ordering, for the named resources only. Attachments that
    /// are not in the list stay in tile memory.
    void memory_barrier_fragment_resources(void* const* resources, u32 count);
    void draw_mesh(u32 object_threadgroups, u32 threads_per_object,
                   u32 threads_per_mesh);
    /// D3D12 DispatchMesh: drawMeshThreadgroups with a 3-D grid and shapes,
    /// or its indirect form when `indirect` is set.
    void draw_mesh_grid(const std::array<u32, 3>& grid, const std::array<u32, 3>& object_shape,
                        const std::array<u32, 3>& mesh_shape, void* indirect, usize indirect_offset);
    void set_object_buffer(void* buffer, usize offset, u32 index);
    // ---- WS5 end ----
    void use_compute_resources(const std::vector<void*>& resources,
                               u32 read_count = 0);
    /// A heap window's residency (MetalResidentToken): declares the log
    /// entries this encoder has not declared yet, once each, so a draw
    /// costs nothing once the encoder has seen its heap.
    void use_render_resident_log(const MetalResidentToken& token);
    void use_compute_resident_log(const MetalResidentToken& token);

    // -- Compute ---
    void begin_compute();
    void end_compute();
    /// Complete prior dispatches and publish all buffer/texture side effects
    /// before another dispatch in the same concurrent compute encoder.
    void memory_barrier_compute();
    void set_compute_pipeline(void* pso);
    void set_compute_buffer(void* buffer, usize offset, u32 index);
    void set_compute_bytes(const void* data, usize length, u32 index);
    void set_compute_texture(void* texture, u32 index);
    void set_compute_sampler(void* sampler, u32 index);
    void dispatch(u32 gx, u32 gy, u32 gz, u32 tx, u32 ty, u32 tz);
    /// GPU-sourced dispatch. `args_buffer` holds three uint32 threadgroup
    /// counts at `args_offset` — the layout D3D's DispatchIndirect reads.
    void dispatch_indirect(void* args_buffer, usize args_offset,
                           u32 tx, u32 ty, u32 tz);

    /// Encode a timeline signal or wait on the open command buffer. Both close
    /// the active encoder first: Metal orders an event against command-buffer
    /// boundaries, not against work inside an encoder.
    void signal_event(void* event, u64 value);
    void wait_for_event(void* event, u64 value);

    // -- Blit ---
    void blit_copy_buffer(void* src, usize src_off, void* dst, usize dst_off, usize size);
    void blit_fill_buffer(void* dst, usize offset, usize size, u8 value);
    void blit_copy_texture(void* src, void* dst, u32 width, u32 height);
    /// Shared staging buffer -> texture level/slice at (x, y, z). `options`
    /// is an MTLBlitOption: which plane of a depth+stencil texture it fills.
    void blit_copy_buffer_to_texture(void* src, usize bytes_per_row,
                                     usize bytes_per_image, u32 width,
                                     u32 height, u32 depth, void* dst,
                                     u32 level, u32 slice, u32 x, u32 y,
                                     u32 z, u32 options = 0,
                                     usize src_offset = 0);
    /// Texture level/slice box at (x, y, z) -> buffer. The inverse of
    /// blit_copy_buffer_to_texture: a byte copy, so it is also how two
    /// textures of different pixel formats exchange texels.
    void blit_copy_texture_to_buffer(void* src, u32 level, u32 slice, u32 x,
                                     u32 y, u32 z, u32 width, u32 height,
                                     u32 depth, void* dst,
                                     usize bytes_per_row,
                                     usize bytes_per_image,
                                     usize dst_offset = 0, u32 options = 0);
    /// Resolve an MSAA color texture into a single-sample texture of the
    /// same size and format. Closes any open render encoder first.
    void resolve_msaa_texture(void* src, void* dst, u32 src_level = 0,
                              u32 src_slice = 0, u32 dst_level = 0,
                              u32 dst_slice = 0);
    void blit_copy_texture_region(void* src, u32 src_x, u32 src_y, void* dst,
                                  u32 dst_x, u32 dst_y, u32 width, u32 height,
                                  u32 src_level = 0, u32 src_slice = 0,
                                  u32 dst_level = 0, u32 dst_slice = 0,
                                  u32 depth = 1, u32 src_z = 0,
                                  u32 dst_z = 0);
    void blit_generate_mipmaps(void* texture);

    // -- Submit ---
    void commit();
    /// Commit and return a retained command buffer (`id<MTLCommandBuffer>`).
    /// Caller owns the retain and must bridge-transfer it to release.
    [[nodiscard]] void* commit_retained();
    /// Commit after installing `on_complete(token)`. The callback runs on
    /// Metal's completion queue and must be thread-safe and non-blocking.
    void commit_with_handler(void (*on_complete)(u64), u64 token);
    /// Commit after installing `on_complete(context, gpu_end_seconds)`, where
    /// the time is the command buffer's GPUEndTime (host CACurrentMediaTime
    /// clock). Runs on Metal's completion queue; must not block.
    void commit_with_end_time(void (*on_complete)(void*, double),
                              void* context);
    void commit_and_wait();

    // -- Debug markers (D3D event markers, GraphicsBackendDevice) ---
    /// Open a named group. Metal balances debug groups per encoder, while a
    /// D3D event spans any number of passes: the open groups are pushed onto
    /// every encoder as it begins and popped before it ends, so each encoder
    /// shows the events it lies in.
    void push_debug_group(const std::string& label);
    /// Close the innermost open group; with none open (an unbalanced
    /// EndEvent), nothing happens.
    void pop_debug_group();
    /// A named point: a signpost in the open encoder, or an empty group
    /// between encoders.
    void insert_debug_signpost(const std::string& label);

    MetalCommandRecorder(const MetalCommandRecorder&) = delete;
    MetalCommandRecorder& operator=(const MetalCommandRecorder&) = delete;

private:
    [[nodiscard]] void* active_encoder() const noexcept {
        return m_render_encoder != nullptr    ? m_render_encoder
               : m_compute_encoder != nullptr ? m_compute_encoder
                                              : m_blit_encoder;
    }
    /// Push every open debug group onto a just-created @p encoder.
    void open_debug_groups(void* encoder);
    /// Pop what open_debug_groups and push_debug_group put on @p encoder,
    /// before it ends.
    void close_debug_groups(void* encoder);
    void end_active_encoder();
    void ensure_blit_encoder();
    void reset_render_state_cache();
    void advance_inline_constant_frame();
    void release_constant_slabs();
    void bind_large_constants(bool vertex, const void* data, usize length,
                              u32 index);
    [[nodiscard]] void* disabled_depth_stencil();
    [[nodiscard]] void* dummy_black_texture();
    [[nodiscard]] void* default_sampler();

    void* m_queue = nullptr;
    void* m_command_buffer = nullptr;
    /// What the current command buffer holds, for gpu_watch (pipeline labels
    /// in encode order, render passes).
    std::string m_cb_summary;
    std::vector<gpu_watch::EventWait> m_cb_waits;
    /// VITREO_GPU_PROFILE: this command buffer's timestamped encoders.
    gpu_profile::RecordingPtr m_profile;
    void* m_render_encoder = nullptr;
    /// Resources already declared on m_render_encoder, and the usage bits
    /// given to useResource. Argument-buffer residency is per encoder, not
    /// per draw: repeating it for every draw is a host call and a GPU hazard
    /// update. Cleared with the render-state cache when the encoder ends.
    void* m_resource_encoder = nullptr;
    std::unordered_map<void*, unsigned> m_resource_usage;
    /// Last use_render_resources list on this encoder. Grass and other
    /// repeated draws name the same resources; the map lookup is the cost.
    std::vector<void*> m_last_use;
    unsigned m_last_use_need{0};
    bool m_last_use_valid{false};
    /// use_*_resident_log: per open encoder, how many entries of each log
    /// it has declared. The log is held so its address cannot be reused by
    /// a newer one while this encoder is open.
    struct DeclaredLog {
        std::shared_ptr<MetalResidentLog> log;
        u32 count{0};
        bool writable{false};
    };
    void* m_log_encoder = nullptr;
    std::unordered_map<const MetalResidentLog*, DeclaredLog> m_declared_logs;
    std::vector<void*> m_log_scratch;
    void use_resident_log(const MetalResidentToken& token, void* encoder,
                          bool render);
    void* m_compute_encoder = nullptr;
    void* m_blit_encoder = nullptr;
    /// The open debug groups, outermost first, and how many of them the
    /// active encoder carries.
    std::vector<std::string> m_debug_groups;
    usize m_encoder_debug_depth = 0;
    /// Groups pushed past kMaxDebugGroups (an application that never ends
    /// its events): counted, so their pops still pair, but not mirrored onto
    /// every encoder.
    static constexpr usize kMaxDebugGroups = 64;
    usize m_debug_overflow = 0;
    /// Always/no-write depth-stencil. AMDMTLBronzeDriver SIGSEGVs when
    /// setDepthStencilState: is passed nil (fault at 0x18); Apple GPUs
    /// treat nil as the default. Draws with no depth attachment must bind
    /// this instead.
    void* m_disabled_depth_stencil = nullptr;
    /// 1x1 transparent-black and a clamp/linear sampler. Binding nil
    /// textures/samplers is defined as black on Apple GPUs; AMD Polaris
    /// samples garbage (park geometry/lightmaps flicker).
    void* m_dummy_black_texture = nullptr;
    void* m_default_sampler = nullptr;

    void* m_render_pipeline = nullptr;
    void* m_depth_stencil = nullptr;
    std::array<f64, 6> m_viewport{};
    std::array<u32, 4> m_scissor{};
    u32 m_cull_mode{0};
    u32 m_triangle_fill_mode{0};
    u32 m_depth_clip_mode{0};
    std::array<f32, 3> m_depth_bias{};
    u32 m_front_facing{0};
    static constexpr u32 kCachedBufferSlots = 32;
    static constexpr u32 kCachedTextureSlots = 16;
    std::array<void*, kCachedBufferSlots> m_vertex_buffers{};
    std::array<usize, kCachedBufferSlots> m_vertex_buffer_offsets{};
    std::array<bool, kCachedBufferSlots> m_vertex_buffer_bound{};
    std::array<u32, kCachedBufferSlots> m_vertex_buffer_strides{};
    std::array<void*, kCachedBufferSlots> m_fragment_buffers{};
    std::array<usize, kCachedBufferSlots> m_fragment_buffer_offsets{};
    std::array<bool, kCachedBufferSlots> m_fragment_buffer_bound{};
    std::array<void*, kCachedTextureSlots> m_vertex_textures{};
    std::array<bool, kCachedTextureSlots> m_vertex_texture_bound{};
    std::array<void*, kCachedTextureSlots> m_vertex_samplers{};
    std::array<bool, kCachedTextureSlots> m_vertex_sampler_bound{};
    std::array<void*, kCachedTextureSlots> m_fragment_textures{};
    std::array<bool, kCachedTextureSlots> m_fragment_texture_bound{};
    std::array<void*, kCachedTextureSlots> m_fragment_samplers{};
    std::array<bool, kCachedTextureSlots> m_fragment_sampler_bound{};
    usize m_vs_bytes_length{0};
    u64 m_vs_bytes_hash{0};
    bool m_vs_bytes_bound{false};
    usize m_ps_bytes_length{0};
    u64 m_ps_bytes_hash{0};
    bool m_ps_bytes_bound{false};
    /// Per-draw constant payloads larger than Metal's 4 KiB set*Bytes copy.
    /// One reused buffer is unsafe: later draws overwrite in-flight GPU reads
    /// (more objects → more stomps → worse geometry at high detail).
    struct ConstantSlab {
        void* buffer{nullptr};
        usize capacity{0};
        usize used{0};
    };
    static constexpr u32 kConstantFrames = 3;
    std::array<std::vector<ConstantSlab>, kConstantFrames> m_vs_constant_slabs;
    std::array<std::vector<ConstantSlab>, kConstantFrames> m_ps_constant_slabs;
    u32 m_constant_frame{0};
    /// GPU generation the slabs of each constant frame were last written for.
    /// Recycling used to key off a 3-Present ring, which is a proxy for "the
    /// GPU is done" and fails whenever the CPU runs more than kConstantFrames
    /// ahead. One guest presents at ~2400 fps, so it wraps continuously and
    /// overwrites constants under draws still in flight. MetalBuffer moved its
    /// discard pool off exactly this heuristic (see g_slot_gen); this is the
    /// same fix for the constant pool.
    std::array<u64, kConstantFrames> m_constant_slot_gen{};
    u32 m_vs_constant_slab{0};
    u32 m_ps_constant_slab{0};
    bool m_render_pipeline_bound{false};
    bool m_depth_stencil_bound{false};
    bool m_viewport_bound{false};
    bool m_scissor_bound{false};
    bool m_cull_mode_bound{false};
    bool m_triangle_fill_mode_bound{false};
    bool m_depth_clip_mode_bound{false};
    bool m_depth_bias_bound{false};
    bool m_front_facing_bound{false};
};

} // namespace vitreo::graphics
