// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_device.hpp
/// @brief Metal GPU device singleton wrapper

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/host_memory_floor.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Forward-declare Metal/QuartzCore types to keep the header C++-friendly.
// Implementation files (.mm) include the real Metal headers.
#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLCommandQueue;
#else
using MTLDevice = void;
using MTLCommandQueue = void;
#endif

namespace vitreo::graphics {

/// Singleton wrapper around MTLDevice and MTLCommandQueue.
/// Provides GPU capability queries used by all frontends.
class MetalDevice final : public GraphicsBackendDevice {
public:
    /// Get the singleton instance (created on first call).
    static MetalDevice& instance();

    // -- Device accessors (Objective-C callers cast to id<MTLDevice>) --------
    void* device_ptr() const;          // Returns id<MTLDevice> as void*
    void* command_queue_ptr() const;   // Returns id<MTLCommandQueue> as void*
    [[nodiscard]] bool available() const noexcept override {
        return m_device != nullptr && m_command_queue != nullptr;
    }
    [[nodiscard]] bool supports_texture_sample_count(
        unsigned count) const override;
    [[nodiscard]] bool supports_float32_filtering() const override;
    [[nodiscard]] bool supports_float32_multisample_resolve() const override;
    [[nodiscard]] u64 max_buffer_length() const override;
    [[nodiscard]] bool supports_framebuffer_fetch() const override;
    [[nodiscard]] bool supports_programmable_sample_positions() const override;
    [[nodiscard]] bool supports_shader_stencil_export() const override;
    [[nodiscard]] bool has_unified_memory() const override;
    [[nodiscard]] bool supports_debug_markers() const override {
        return available();
    }
    void push_debug_group(const std::string& label) override;
    void pop_debug_group() override;
    void insert_debug_signpost(const std::string& label) override;
    [[nodiscard]] bool supports_tiled_resources() const override;
    [[nodiscard]] bool tiled_texture_tile_shape(PixelFormat format,
                                                unsigned samples,
                                                unsigned& width,
                                                unsigned& height) const override;
    [[nodiscard]] std::unique_ptr<BackendTilePool> create_tile_pool() override;
    [[nodiscard]] bool tiled_texture_layout(
        const BackendTexture2D* texture, BackendTiledLayout& layout) override;
    [[nodiscard]] bool update_tile_mappings(BackendTexture2D* texture,
                                            BackendTilePool* pool,
                                            const BackendTileMapping* mappings,
                                            usize count,
                                            bool self_aliased) override;
    [[nodiscard]] bool update_tile_mappings(BackendBuffer* buffer,
                                            BackendTilePool* pool,
                                            const BackendTileMapping* mappings,
                                            usize count,
                                            bool self_aliased) override;
    [[nodiscard]] bool tiled_resource_barrier() override;
    [[nodiscard]] bool copy_texture_buffer_region(
        BackendTexture2D* texture, unsigned level, unsigned slice, unsigned x,
        unsigned y, unsigned width, unsigned height, BackendBuffer* buffer,
        u64 offset, u64 row_pitch, bool to_texture) override;
    [[nodiscard]] bool copy_texture_3d_buffer_region(
        BackendTexture3D* texture, unsigned level, unsigned x, unsigned y,
        unsigned z, unsigned width, unsigned height, unsigned depth,
        BackendBuffer* buffer, u64 offset, u64 row_pitch, u64 slice_pitch,
        bool to_texture) override;
    [[nodiscard]] bool supports_command_lists() const noexcept override;
    [[nodiscard]] std::unique_ptr<BackendCommandList> create_command_list()
        override;
    [[nodiscard]] bool submit_command_list(BackendCommandList* list) override;
    [[nodiscard]] bool supports_fences() const noexcept override;
    [[nodiscard]] std::unique_ptr<BackendFence> create_fence(
        u64 initial_value) override;
    [[nodiscard]] bool supports_descriptor_tables() const noexcept override;
    [[nodiscard]] std::unique_ptr<BackendDescriptorTable>
    create_descriptor_table(const BackendDescriptorTableDesc& desc) override;
    [[nodiscard]] std::unique_ptr<BackendSamplerState>
    create_sampler_state(const BackendSamplerDesc& desc) override;
    [[nodiscard]] std::unique_ptr<BackendDepthStencilState>
    create_depth_stencil_state(const BackendDepthStencilDesc& desc) override;
    [[nodiscard]] std::string backend_device_name() const override {
        return m_device_name;
    }
    [[nodiscard]] void* native_device() const noexcept override {
        return m_device;
    }
    [[nodiscard]] void* native_command_queue() const noexcept override {
        return m_command_queue;
    }
    [[nodiscard]] std::unique_ptr<BackendBuffer>
    create_buffer(const BackendBufferDesc& desc,
                  const void* initial_data) override;
    [[nodiscard]] std::unique_ptr<BackendTexture2D>
    create_texture_2d(const BackendTexture2DDesc& desc,
                      const void* initial_data,
                      unsigned initial_row_pitch) override;
    [[nodiscard]] std::unique_ptr<BackendTexture3D>
    create_texture_3d(const BackendTexture3DDesc& desc,
                      const void* initial_data, unsigned initial_row_pitch,
                      unsigned initial_slice_pitch) override;
    [[nodiscard]] u32 simd_group_width() const override;
    [[nodiscard]] bool supports_placement_heaps() const noexcept override;
    [[nodiscard]] std::unique_ptr<BackendHeap> create_heap(
        const BackendHeapDesc& desc) override;
    [[nodiscard]] bool heap_texture_placement(
        const BackendTexture2DDesc& desc, u64& size,
        u64& alignment) override;
    [[nodiscard]] bool heap_texture_placement(
        const BackendTexture3DDesc& desc, u64& size,
        u64& alignment) override;
    [[nodiscard]] std::unique_ptr<BackendTextureBuffer>
    create_texture_buffer(const BackendBuffer& buffer, PixelFormat pixel_format,
                          unsigned first_element, unsigned num_elements,
                          bool shader_write = false) override;
    [[nodiscard]] std::unique_ptr<BackendTextureBuffer>
    create_mapped_texture_buffer(const BackendBuffer& buffer,
                                 PixelFormat pixel_format,
                                 unsigned first_element, unsigned num_elements,
                                 u16 component_mapping) override;
    [[nodiscard]] std::unique_ptr<BackendTexture2D>
    open_shared_texture_2d(u64 handle) override;
    [[nodiscard]] bool supports_shared_texture_2d(
        PixelFormat format) const noexcept override;
    [[nodiscard]] std::shared_ptr<void> retain_shared_texture(
        u64 handle) override;
    [[nodiscard]] bool supports_shared_texture(
        PixelFormat format) const noexcept override;
    [[nodiscard]] bool supports_extended_sharing() const noexcept override {
        return m_device != nullptr;
    }
    [[nodiscard]] std::shared_ptr<BackendSharedMemory> create_shared_memory(
        usize bytes, std::vector<u8> metadata) override;
    [[nodiscard]] SharedOpening open_shared_resource(u64 handle) override;
    [[nodiscard]] std::shared_ptr<GpuCompletion> publish_shared_writes()
        override;
    void acquire_shared_writes() override;
    [[nodiscard]] bool copy_texture_to_shared_memory(
        const BackendTextureLocation& src, unsigned width, unsigned height,
        unsigned depth, BackendSharedMemory& dst, usize offset,
        usize row_pitch, usize image_pitch) override;
    [[nodiscard]] bool copy_shared_memory_to_texture(
        BackendSharedMemory& src, usize offset, usize row_pitch,
        usize image_pitch, const BackendTextureLocation& dst, unsigned width,
        unsigned height, unsigned depth) override;
    [[nodiscard]] std::unique_ptr<BackendSwapChain>
    create_swap_chain(const BackendSwapChainDesc& desc) override;
    [[nodiscard]] std::unique_ptr<BackendPipeline>
    create_pipeline(const BackendPipelineDesc& desc) override;
    void prefetch_shader(const std::string& msl,
                         const std::string& entry_point,
                         BackendMathMode math_mode,
                         bool preserve_invariance) override;
    [[nodiscard]] std::unique_ptr<BackendComputePipeline>
    create_compute_pipeline(const BackendComputePipelineDesc& desc) override;
    [[nodiscard]] std::unique_ptr<BackendRenderPass>
    create_render_pass_standalone(const BackendRenderPassDesc& desc) override;
    [[nodiscard]] bool dispatch(const BackendDispatchCall& call) override;
    [[nodiscard]] bool copy_buffer(BackendBuffer* dst,
                                   const BackendBuffer* src) override;
    [[nodiscard]] bool copy_buffer_region(BackendBuffer* dst, u64 dst_offset,
                                          const BackendBuffer* src,
                                          u64 src_offset,
                                          u64 bytes) override;
    [[nodiscard]] bool fill_buffer_region(BackendBuffer* dst, u64 offset,
                                          u64 bytes, u8 value) override;
    [[nodiscard]] bool copy_buffer_texture_row(BackendBuffer* buffer,
                                               u64 offset,
                                               BackendTexture2D* texture,
                                               unsigned width,
                                               bool to_texture) override;
    [[nodiscard]] bool copy_texture_2d(BackendTexture2D* dst,
                                       const BackendTexture2D* src) override;
    [[nodiscard]] bool resolve_texture_2d(
        BackendTexture2D* dst, unsigned dst_mip, unsigned dst_slice,
        const BackendTexture2D* src, unsigned src_mip,
        unsigned src_slice, PixelFormat resolve_format) override;
    [[nodiscard]] bool copy_texture_2d_region(
        BackendTexture2D* dst, unsigned dst_x, unsigned dst_y,
        const BackendTexture2D* src, unsigned src_x, unsigned src_y,
        unsigned width, unsigned height, unsigned dst_mip = 0,
        unsigned dst_slice = 0, unsigned src_mip = 0,
        unsigned src_slice = 0) override;
    [[nodiscard]] bool stretch_texture_2d_region(
        BackendTexture2D* dst, unsigned dst_x, unsigned dst_y,
        unsigned dst_width, unsigned dst_height, const BackendTexture2D* src,
        unsigned src_x, unsigned src_y, unsigned src_width,
        unsigned src_height, BackendFilter filter, unsigned dst_slice = 0,
        unsigned src_slice = 0) override;
    [[nodiscard]] bool copy_texture_elements(
        const BackendTextureLocation& dst, const BackendTextureLocation& src,
        unsigned width, unsigned height, unsigned depth) override;
    [[nodiscard]] bool generate_mips(BackendTexture2D* texture) override;
    [[nodiscard]] bool generate_mips_window(BackendTexture2D* texture,
                                            unsigned first_mip,
                                            unsigned mip_count,
                                            unsigned first_slice,
                                            unsigned slice_count) override;
    [[nodiscard]] bool generate_mips(BackendTexture3D* texture) override;
    [[nodiscard]] bool copy_texture_3d(BackendTexture3D* dst,
                                       const BackendTexture3D* src) override;
    void flush() override;
    [[nodiscard]] bool wants_early_submit() const override;
    [[nodiscard]] std::shared_ptr<GpuCompletion> flush_completion() override;
    [[nodiscard]] std::shared_ptr<GpuTimestamp> record_timestamp() override;
    [[nodiscard]] std::shared_ptr<GpuOcclusionCounter>
    create_occlusion_counter() override;
    /// Tests only: called on the recording thread around the BeginPass of a
    /// render pass that counts into a GpuOcclusionCounter -- @p after false
    /// just before the BeginPass is recorded, true just after it and before
    /// the counter takes its generation -- so a test can make another thread
    /// submit at exactly those points. nullptr: none. Change it only while no
    /// pass is being created.
    using OcclusionPassHook = void (*)(bool after, void* context);
    static void set_occlusion_pass_hook_for_test(OcclusionPassHook hook,
                                                 void* context) noexcept;
    /// Tests only: passes bound to an occlusion counter from now on count
    /// without MTLVisibilityResultTypeAccumulate -- a slot per pass, the
    /// macOS 13 path -- on any macOS. Change it only while no pass is being
    /// created.
    static void set_occlusion_reset_visibility_for_test(bool reset) noexcept;
    [[nodiscard]] u64 recording_generation() override;
    [[nodiscard]] u64 last_recorded_generation() override;
    [[nodiscard]] BackendDrawEncodeCounts draw_encode_counts() const override;
    [[nodiscard]] bool generation_complete(u64 generation) override;
    [[nodiscard]] bool wait_generation(u64 generation) override;
    [[nodiscard]] bool wait_idle() override;
    void set_present_gamma_ramp(const u16* rgb_256x3) override;
    [[nodiscard]] bool present_gamma_active() const noexcept;
    [[nodiscard]] void* present_gamma_lut() const noexcept;
    [[nodiscard]] void* present_gamma_compute_pso() const noexcept;
    /// The texel-fill kernels behind BackendTexture{2D,3D}::fill_texels:
    /// `volume` selects the texture3d form over texture2d_array. Built once;
    /// null if Metal refused them.
    [[nodiscard]] void* fill_texels_pipeline(bool volume);

    // -- Capability queries --------------------------------------------------
    [[nodiscard]] bool query_video_memory(BackendVideoMemoryInfo& info) const noexcept override;
    /// This device's allocation against Metal's recommended working set.
    [[nodiscard]] memory::GpuWorkingSet gpu_working_set() const noexcept;
    u32 max_texture_size() const       { return m_max_texture_size; }
    u64 max_buffer_size() const        { return m_max_buffer_size; }
    [[nodiscard]] u64 recommended_working_set_size() const noexcept override {
        return m_recommended_working_set_size;
    }
    u32 max_threads_per_threadgroup() const { return m_max_threads; }
    bool supports_mesh_shaders() const { return m_mesh_shaders; }
    bool supports_argument_buffers_tier2() const { return m_arg_buf_tier2; }
    bool supports_32bit_msaa() const   { return m_msaa_32bit; }
    std::string device_name() const    { return m_device_name; }

    // Non-copyable
    MetalDevice(const MetalDevice&) = delete;
    MetalDevice& operator=(const MetalDevice&) = delete;

private:
    MetalDevice();
    ~MetalDevice();

    [[nodiscard]] bool ensure_stretch_blit(PixelFormat format,
                                           unsigned sample_count = 1);
    void ensure_present_gamma_resources();

    void* m_device = nullptr;          // id<MTLDevice>
    void* m_command_queue = nullptr;   // id<MTLCommandQueue>
    std::unique_ptr<BackendBuffer> m_stretch_vb;
    std::unordered_map<u32, std::unique_ptr<BackendPipeline>>
        m_stretch_pipelines;

    // Cached capabilities
    u32  m_max_texture_size = 0;
    u64  m_max_buffer_size = 0;
    u64  m_recommended_working_set_size = 0;
    u32  m_max_threads = 0;
    bool m_mesh_shaders = false;
    bool m_arg_buf_tier2 = false;
    bool m_msaa_32bit = false;
    std::string m_device_name;

    void* m_gamma_lut = nullptr;
    void* m_gamma_compute_pso = nullptr;
    std::mutex m_fill_mutex;
    void* m_fill_pso_2d = nullptr;
    void* m_fill_pso_3d = nullptr;
    bool m_fill_tried = false;
    std::atomic<bool> m_gamma_active{false};
};

} // namespace vitreo::graphics
