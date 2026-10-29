// SPDX-License-Identifier: LGPL-2.1-only
// VITREO_LARGE_FILE_OK: one state struct shared by every fake-backend class; splitting it would rename members
#pragma once
/// @file d3d11_test_backend_state.hpp
/// @brief The fake graphics backend's observable state (BackendState) and the hand-completed GPU points.

#include "d3d11_test_common.hpp"

namespace vitreo::winapi::d3d11_test {

/// A command-stream point the test completes by hand, standing in for the
/// GPU reaching a recorded query End.
struct TestGpuPoint final : graphics::GpuTimestamp {
    bool is_ready{false};
    u64 ns{0};
    bool ready() const override { return is_ready; }
    u64 gpu_time_ns() const override { return ns; }
};

/// An occlusion counter whose sample count the test sets, standing in for
/// the GPU having counted the draws of one interval. Like Metal's, it takes
/// the generation of each pass created with it once the pass is recorded
/// (create_render_pass_standalone).
struct TestOcclusionCounter final : graphics::GpuOcclusionCounter {
    u64 count{0};
    u64 generation{0};
    u64 samples() const override { return count; }
    u64 recorded_generation() const override { return generation; }
};

class TestBackendBuffer;

struct BackendState {
    /// fill_depth_stencil_texels, in order: {depth bits, stencil value or
    /// ~0u when not written, mip, first slice, slices, x, y, width, height}.
    std::vector<std::array<u32, 9>> depth_fills;
    /// The debug markers the frontend emitted: "push:<label>", "pop",
    /// "sign:<label>".
    std::vector<std::string> debug_markers;
    bool debug_markers_supported{true};
    bool unified_memory{false};
    /// Every buffer create_buffer made, for a test that plays the GPU writing
    /// one (live_buffers).
    std::vector<TestBackendBuffer*> created_buffers;
    std::vector<std::shared_ptr<TestGpuPoint>> gpu_points;
    /// Every occlusion counter created, in order, and the counter each render
    /// pass was created with (null when it counted nothing).
    std::vector<std::shared_ptr<TestOcclusionCounter>> occlusion_counters;
    std::vector<const graphics::GpuOcclusionCounter*> pass_occlusion_counters;
    /// Run when a pass that counts into an occlusion counter is created,
    /// before it is recorded: a test plays another thread submitting there.
    std::function<void()> before_counted_pass;
    /// Run when a draw is about to be recorded into a pass: a test plays
    /// another thread submitting between the frontend's preparation of the
    /// draw (its statistics record, its occlusion counter) and its record.
    std::function<void()> before_draw;
    // Stream output: capture draws, the repack dispatch, and pipelines built
    // with rasterization off.
    unsigned capture_draws{0};
    /// R3c native states (delta_record.hpp): the states the backend built,
    /// the draws that named one, and draws naming a state built from a desc
    /// other than the one the draw carries (must stay 0).
    unsigned sampler_states_created{0};
    unsigned depth_stencil_states_created{0};
    unsigned draws_naming_depth_stencil_state{0};
    unsigned draws_naming_sampler_state{0};
    unsigned native_state_mismatches{0};
    const graphics::BackendDepthStencilState* last_depth_stencil_state{nullptr};
    unsigned last_capture_offset{0};
    unsigned last_capture_vertex_count{0};
    unsigned last_capture_first_instance{0};
    const graphics::BackendBuffer* last_capture_buffer{nullptr};
    unsigned rasterization_disabled_pipelines{0};
    // Geometry emulation: pass C is an indirect list draw reading t0.
    unsigned indirect_draws{0};
    graphics::BackendPrimitiveTopology last_draw_topology{};
    const graphics::BackendBuffer* last_draw_vertex_srv0{nullptr};
    // Input-assembler streams of the last draw and pipeline, by backend
    // stream index.
    std::array<const graphics::BackendBuffer*,
               graphics::BACKEND_MAX_VERTEX_BUFFERS>
        last_draw_vertex_buffers{};
    std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS>
        last_draw_vertex_buffer_offsets{};
    std::vector<graphics::BackendVertexAttribute> last_pipeline_attributes;
    std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS>
        last_pipeline_strides{};
    const graphics::BackendBuffer* last_draw_vertex_srv1{nullptr};
    unsigned last_draw_vertex_count{0};
    unsigned last_draw_patch_count{0};
    unsigned last_draw_patch_control_points{0};
    const graphics::BackendBuffer* last_draw_tess_factors{nullptr};
    std::array<const graphics::BackendBuffer*,
               graphics::BACKEND_MAX_COMPUTE_BUFFERS> last_dispatch_buffers{};
    /// What each of those buffers held when the dispatch was recorded
    /// (transient buffers can be gone by the time a test looks).
    std::array<std::vector<u8>, graphics::BACKEND_MAX_COMPUTE_BUFFERS>
        last_dispatch_buffer_bytes{};
    std::array<unsigned, graphics::BACKEND_MAX_COMPUTE_BUFFERS>
        last_dispatch_buffer_offsets{};
    std::array<const graphics::BackendTextureBuffer*,
               graphics::BACKEND_MAX_COMPUTE_TEXTURES>
        last_dispatch_texture_buffers{};
    std::vector<std::vector<u32>> repack_controls;
    /// Buffers keep real contents (update / copy_buffer_region write them,
    /// read returns them), and the stream-output accounting kernels run
    /// through their C++ models (graphics::stream_out_prepare_model,
    /// stream_out_draw_auto_count) -- the GPU half the frontend now relies
    /// on for every count. Off: read() returns its old test pattern.
    bool live_buffers{false};
    /// Each vitreo_so_prepare: its constants and the op record it wrote.
    std::vector<graphics::SoPrepareParams> prepare_params;
    std::vector<std::array<u32, graphics::SO_OP_WORDS>> prepare_ops;
    /// Each vitreo_so_drawauto: the vertex count it wrote.
    std::vector<u32> drawauto_counts;
    /// Tessellation (patch-draw) pipelines created, and the last one's mode.
    unsigned tessellation_pipelines{0};
    graphics::BackendTessellationPartition last_tessellation_partition{};
    /// Every dispatch's entry point, in order, and whether it was indirect.
    std::vector<std::string> dispatched_entries;
    unsigned indirect_dispatches{0};
    /// The last dispatch's push block (BackendDispatchCall::constants).
    std::vector<u32> last_dispatch_constants;
    unsigned buffer_creations{0};
    unsigned buffer_updates{0};
    graphics::BufferWrite last_buffer_update_mode{};
    unsigned texture_creations{0};
    unsigned swap_chain_creations{0};
    /// The presentation view the newest backend chain was attached to.
    void* last_swap_chain_view{nullptr};
    unsigned texture_clears{0};
    unsigned last_texture_clear_mip{0};
    unsigned last_texture_clear_slice{0};
    unsigned last_texture_clear_array_size{1};
    unsigned depth_clears{0};
    unsigned last_depth_clear_mip{0};
    unsigned last_depth_clear_slice{0};
    unsigned last_depth_clear_array_size{1};
    bool last_depth_clear_depth{false};
    bool last_depth_clear_stencil{false};
    unsigned back_buffer_clears{0};
    unsigned presents{0};
    unsigned last_present_sync{0};
    bool present_would_block{false};
    graphics::BackendColorSpace last_color_space{
        graphics::BackendColorSpace::Srgb};
    unsigned resizes{0};
    unsigned flushes{0};
    /// What wants_early_submit() answers: the GPU is running out of work.
    bool gpu_wants_work{false};
    /// Called at the start of every create_buffer, on the calling thread.
    std::function<void()> on_create_buffer;
    unsigned buffer_copies{0};
    /// One entry per copy_buffer_region: destination offset, source offset,
    /// byte count, and the size of the source buffer -- which is how a test
    /// tells a UAV's four-byte hidden counter from the resource it counts.
    struct BufferRegionCopy {
        u64 dst_offset{0};
        u64 src_offset{0};
        u64 bytes{0};
        usize src_byte_width{0};
        /// The source's creation data, when it was a TestBackendBuffer given
        /// some: identifies a transient buffer holding a value to store.
        std::vector<u8> src_contents;
    };
    std::vector<BufferRegionCopy> buffer_region_copies;
    /// One entry per accepted fill_buffer_region: offset, byte count, value.
    struct BufferFill {
        u64 offset{0};
        u64 bytes{0};
        u8 value{0};
    };
    std::vector<BufferFill> buffer_fills;
    /// copy_buffer_texture_row calls: a buffer render-target view's window
    /// moving into (to_texture) or out of its proxy.
    struct BufferTextureRowCopy {
        u64 offset{0};
        unsigned width{0};
        bool to_texture{false};
        graphics::PixelFormat texture_format{graphics::PixelFormat::Unknown};
        unsigned draws_before{0};
    };
    std::vector<BufferTextureRowCopy> buffer_texture_row_copies;
    /// Colour target 0 of the last render pass (width, height, format).
    graphics::BackendTexture2DDesc last_pass_color0_desc{};
    /// Bytes of vertex t1 / t2 the last draw bound (a line-quad pass C's
    /// record ids and LineQuadParams).
    std::vector<u8> last_draw_vertex_srv1_bytes;
    std::vector<u8> last_draw_vertex_srv2_bytes;
    /// The pixel format of the volume the last dispatch bound at u0.
    graphics::PixelFormat last_dispatch_uav_3d_format{
        graphics::PixelFormat::Unknown};
    unsigned last_dispatch_uav_3d_mips{0};
    unsigned texture_copies{0};
    unsigned texture_3d_copies{0};
    unsigned texture_resolves{0};
    unsigned texture_region_copies{0};
    unsigned last_copy_dst_mip{0};
    unsigned last_copy_dst_slice{0};
    unsigned last_copy_src_mip{0};
    unsigned last_copy_src_slice{0};
    graphics::PixelFormat last_resolve_format{graphics::PixelFormat::Unknown};
    unsigned last_copy_width{0};
    unsigned last_copy_height{0};
    std::vector<std::array<unsigned, 4>> copied_subresources;
    /// {dst_x, dst_y, src_x, src_y, width, height} per region copy.
    std::vector<std::array<unsigned, 6>> copy_regions;
    unsigned texture_updates{0};
    unsigned texture_native_readbacks{0};
    unsigned mip_generations{0};
    /// Area 06 recordings. One entry per fill_texels (2D: z/depth are the
    /// array slice window; 3D: the W box), texel bytes included.
    struct TexelFill {
        std::vector<u8> texel;
        unsigned mip{0};
        unsigned x{0}, y{0}, z{0};
        unsigned width{0}, height{0}, depth{0};
        bool volume{false};
    };
    std::vector<TexelFill> texel_fills;
    /// clear_render_target_view: the view format each clear asked for.
    std::vector<graphics::PixelFormat> clear_view_formats;
    /// copy_texture_elements: {dst mip, slice, x, y, z, src mip, slice, x,
    /// y, z, width, height, depth} and whether both sides are volumes.
    std::vector<std::array<unsigned, 13>> element_copies;
    /// generate_mips_window: {first mip, mip count, first slice, slices}.
    std::vector<std::array<unsigned, 4>> mip_windows;
    unsigned storage_discards{0};
    /// readback_slice_async: off by default (the synchronous path); when on,
    /// every readback reports ready() only once this is set, and fills its
    /// rows with the byte 0x5A.
    /// update_depth_stencil_plane: the last depth and stencil planes.
    std::vector<float> uploaded_depth_plane;
    std::vector<u8> uploaded_stencil_plane;
    bool async_readbacks{false};
    bool readbacks_ready{false};
    unsigned readback_waits{0};
    /// BackendTexture3D::update_region: {mip, x, y, z, w, h, d, row, slice}.
    std::vector<std::array<unsigned, 9>> volume_region_updates;
    std::vector<u8> last_volume_update;
    unsigned texture_readbacks{0};
    unsigned shared_texture_opens{0};
    unsigned pipeline_creations{0};
    unsigned compute_pipeline_creations{0};
    unsigned dispatches{0};
    std::array<unsigned, 3> last_dispatch_groups{};
    std::array<unsigned, 3> last_dispatch_threads{};
    bool last_compute_pipeline_has_msl{false};
    bool last_compute_entry_is_main0{false};
    /// The MSL of the last compute / render pipeline created.
    std::string last_compute_msl;
    std::string last_pipeline_vertex_msl;
    std::string last_pipeline_fragment_msl;
    bool last_dispatch_has_uav{false};
    bool last_dispatch_has_uav_3d{false};
    unsigned last_dispatch_uav_mip{~0u};
    bool last_dispatch_has_srv{false};
    bool last_dispatch_has_sampler{false};
    bool last_dispatch_has_constant_buffer{false};
    unsigned draws{0};
    unsigned wait_idles{0};
    bool wait_idle_ok{false};
    /// Submission generations (off: the backend reports none, like the
    /// contract's default). recording_gen is what new work joins;
    /// flush_completion submits it. completed_gen is what the "GPU" retired.
    bool generations{false};
    u64 recording_gen{1};
    u64 completed_gen{0};
    std::vector<u64> generation_waits;
    /// BackendBuffer::read calls; each fills bytes with 0xC0 ^ position.
    unsigned buffer_reads{0};
    bool force_shared_handle{false};
    bool unique_shared_handles{false};
    /// Shared storage by handle (the IOSurface model): what the creator
    /// stored, which every open_shared_texture_2d of that handle returns.
    std::map<u64, graphics::BackendTexture2DDesc> shared_storage;
    /// The Metal backend's extended sharing (every shape, buffers, shared
    /// CPU memory): off unless a test turns it on.
    bool extended_sharing{false};
    std::map<u64, std::shared_ptr<graphics::BackendSharedMemory>>
        shared_memories;
    std::map<u64, graphics::BackendBufferDesc> shared_buffers;
    std::map<u64, graphics::BackendTexture3DDesc> shared_volumes;
    unsigned shared_publications{0};
    unsigned shared_acquires{0};
    u64 next_shared_id{0x520000u};
    std::chrono::milliseconds wait_idle_delay{0};
    bool last_draw_mipmaps_enabled{true};
    bool last_draw_comparison_enabled{false};
    graphics::BackendCompare last_draw_sampler_comparison{
        graphics::BackendCompare::Never};
    graphics::BackendFilter last_draw_min_filter{
        graphics::BackendFilter::Nearest};
    graphics::BackendFilter last_draw_mag_filter{
        graphics::BackendFilter::Nearest};
    graphics::BackendFilter last_draw_mip_filter{
        graphics::BackendFilter::Nearest};
    unsigned last_draw_max_anisotropy{1};
    graphics::BackendAddressMode last_draw_address_u{
        graphics::BackendAddressMode::Wrap};
    graphics::BackendAddressMode last_draw_address_v{
        graphics::BackendAddressMode::Wrap};
    graphics::BackendAddressMode last_draw_address_w{
        graphics::BackendAddressMode::Wrap};
    u32 last_draw_sampler_border_color{0};
    graphics::BackendFillMode last_draw_fill_mode{
        graphics::BackendFillMode::Fill};
    graphics::BackendCullMode last_draw_cull_mode{
        graphics::BackendCullMode::None};
    bool last_draw_depth_clip_enabled{true};
    float last_draw_depth_bias{0.0f};
    float last_draw_depth_bias_slope_scale{0.0f};
    float last_draw_depth_bias_clamp{0.0f};
    bool last_draw_scissor_enabled{false};
    /// The whole last draw call. Its pointers are only valid during the
    /// draw; tests read the values, and the index bytes copied below.
    graphics::BackendDrawCall last_draw_call{};
    std::vector<u8> last_draw_index_bytes;
    /// Bytes of each vertex stream the last draw bound (a zero stream shows
    /// up as all zeros).
    std::array<std::vector<u8>, graphics::BACKEND_MAX_VERTEX_BUFFERS>
        last_draw_vertex_stream_bytes{};
    /// ByteWidth of each stream the last draw bound (a transient stream is
    /// gone once the draw returns, so its pointer must not be read later).
    std::array<usize, graphics::BACKEND_MAX_VERTEX_BUFFERS>
        last_draw_vertex_stream_widths{};
    graphics::BackendPipelineDesc last_draw_pipeline_desc{};
    /// What supports_framebuffer_fetch and
    /// supports_programmable_sample_positions answer.
    bool framebuffer_fetch{false};
    bool programmable_sample_positions{false};
    /// What supports_shader_stencil_export answers.
    bool shader_stencil_export{false};
    bool last_pass_center_sample_positions{false};
    usize last_capture_buffer_bytes{0};
    std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS>
        last_capture_vertex_offsets{};
    /// The last capture draw itself, its pipeline, and the first 16 bytes of
    /// its capture buffer as recorded (a draw-relative capture's header).
    graphics::BackendDrawCall last_capture_call{};
    graphics::BackendPipelineDesc last_capture_pipeline_desc{};
    std::vector<u8> last_capture_header;
    float last_draw_lod_min_clamp{0.0f};
    float last_draw_lod_max_clamp{0.0f};
    float last_draw_lod_bias{0.0f};
    float last_draw_shader_lod_bias{0.0f};
    usize last_draw_fragment_constants_size{0};
    /// The stage push blocks' bytes, so a test can read the buffer-view
    /// sizes the out-of-bounds guard compares against.
    std::vector<u8> last_draw_vertex_constants;
    std::vector<u8> last_draw_fragment_constants;
    bool last_draw_depth_write_enabled{false};
    bool last_draw_depth_test_enabled{false};
    graphics::BackendCompare last_draw_depth_compare{
        graphics::BackendCompare::Never};
    bool last_draw_texture_bound{false};
    bool last_draw_vertex_texture_bound{false};
    /// Which fragment u# slots the draw arrived with, so a test can assert the
    /// binding survived the frontend rather than only that the draw happened.
    std::array<bool, graphics::BACKEND_MAX_TEXTURE_SLOTS>
        last_draw_uav_texture_bound{};
    std::array<bool, graphics::BACKEND_MAX_TEXTURE_SLOTS>
        last_draw_uav_buffer_bound{};
    std::array<const graphics::BackendTextureBuffer*,
               graphics::BACKEND_MAX_TEXTURE_SLOTS>
        last_draw_uav_texture_buffers{};
    /// How many times a command list has been submitted, and the most recently
    /// created one, so a test can tell recording from executing.
    unsigned list_submissions{0};
    graphics::BackendCommandList* last_command_list{nullptr};
    bool last_draw_stencil_enabled{false};
    u8 last_draw_stencil_read_mask{0};
    u8 last_draw_stencil_write_mask{0};
    u32 last_draw_stencil_reference{0};
    graphics::BackendStencilFaceDesc last_draw_front_stencil{};
    graphics::BackendStencilFaceDesc last_draw_back_stencil{};
    float last_depth_clear_value{-1.0f};
    u8 last_stencil_clear_value{0};
    unsigned last_pass_color_targets{0};
    const graphics::BackendTexture2D* last_pass_color0{nullptr};
    bool last_pass_has_depth{false};
    unsigned last_pass_attachmentless_width{0};
    unsigned last_pass_attachmentless_height{0};
    unsigned last_pass_attachmentless_samples{0};
    bool last_pipeline_attachmentless{false};
    unsigned last_pass_depth_level{0};
    unsigned last_pass_depth_slice{0};
    unsigned last_pass_depth_array_size{1};
    bool last_pass_volume_target0{false};
    unsigned last_pass_depth_plane0{0};
    unsigned last_pass_color_array_size0{1};
    bool last_pipeline_fragment_empty{false};
    std::array<graphics::BackendTextureViewDesc,
               graphics::BACKEND_MAX_TEXTURE_SLOTS> last_texture_views{};
    std::array<graphics::BackendTextureViewDesc,
               graphics::BACKEND_MAX_TEXTURE_SLOTS>
        last_vertex_texture_views{};
    graphics::BackendSamplerDesc last_vertex_sampler{};
    graphics::BackendBufferDesc last_buffer_desc{};
    graphics::BackendTexture2DDesc last_texture_desc{};
    usize last_buffer_update_offset{0};
    std::vector<u8> last_buffer_initial_data;
    std::vector<u8> last_buffer_update;
    u32 last_texture_update_row_pitch{0};
    std::array<unsigned, 6> last_texture_update_region{};
    std::vector<u8> last_texture_update;
    std::vector<std::array<unsigned, 2>> texture_subresource_updates;
    std::vector<u8> texture_subresource_first_bytes;
    std::vector<u8> native_texture_readback;
    std::vector<u8> readback_rgba;
    std::array<float, 4> last_clear{};
    std::vector<graphics::BackendCompositionLayerDesc> composition_layers;
    /// Typed buffer views: the format and element window each was created for.
    struct TextureBufferCreation {
        graphics::PixelFormat pixel_format{graphics::PixelFormat::Unknown};
        unsigned first_element{0};
        unsigned num_elements{0};
        bool shader_write{false};
    };
    std::vector<TextureBufferCreation> texture_buffer_creations;
    graphics::BackendTexture3DDesc last_texture_3d_desc{};
    u32 texture_3d_creations{0};
    // Tiled resources (d3d11_tiled.cpp). Off unless a test turns it on
    // before the device first answers a capability query.
    bool tiled_resources{false};
    bool tile_pool_out_of_memory{false};
    unsigned tiled_barriers{0};
    unsigned tile_mapping_updates{0};
    bool last_self_aliased{false};
    /// Every tiled resource's current mappings, by backend object:
    /// (level, slice, x, y, packed) -> pool tile.
    using TileKey = std::tuple<u32, u32, u32, u32, bool>;
    std::map<const void*, std::map<TileKey, u64>> tile_mappings;
    /// Pool references, by backend pool object: pool tile -> count, and the
    /// size set_size last gave.
    std::map<const void*, std::map<u64, u32>> tile_pool_refs;
    std::map<const void*, u64> tile_pool_sizes;
    struct TileCopy {
        const void* texture{nullptr};
        unsigned level{0}, slice{0}, x{0}, y{0}, width{0}, height{0};
        const void* buffer{nullptr};
        u64 offset{0};
        u64 row_pitch{0};
        bool to_texture{false};
        /// The buffer's bytes when the copy was recorded (to_texture).
        std::vector<u8> bytes;
    };
    std::vector<TileCopy> tile_copies;
    /// The backend object of the tiled resource created last.
    const void* last_tiled_object{nullptr};
};

}  // namespace vitreo::winapi::d3d11_test
