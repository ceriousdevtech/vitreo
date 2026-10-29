// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_device.hpp
/// @brief Shared native graphics-backend device contract.
///
/// Guest API frontends (DirectX, Vulkan, and OpenGL) remain independent, but
/// they select and allocate work through one process-wide backend device. On
/// macOS the concrete implementation is MetalDevice; no frontend translates
/// through another guest API.

#include "graphics/contracts/buffer_write.hpp"
#include "graphics/contracts/format_converter.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "graphics/contracts/backend_command_stream.hpp"
#include "graphics/contracts/backend_render_state.hpp"
#include "graphics/contracts/backend_pipeline.hpp"
#include "graphics/contracts/backend_bindings.hpp"
#include "graphics/contracts/backend_draw_call.hpp"
#include "graphics/contracts/backend_render_pass.hpp"
#include "graphics/contracts/backend_buffer.hpp"
#include "graphics/contracts/backend_memory.hpp"
#include "graphics/contracts/backend_texture_2d.hpp"
#include "graphics/contracts/backend_texture_3d.hpp"
#include "graphics/contracts/backend_swap_chain.hpp"

namespace vitreo::graphics {

struct BackendVideoMemoryInfo {
    u64 budget{0};
    u64 current_usage{0};
};

class GraphicsBackendDevice {
public:
    virtual ~GraphicsBackendDevice() = default;

    [[nodiscard]] virtual bool available() const noexcept = 0;
    /// Whether the device can create a 2D multisample texture with this
    /// sample count. `1` is always supported.
    [[nodiscard]] virtual bool supports_texture_sample_count(
        unsigned count) const {
        return count <= 1;
    }
    /// Whether 32-bit float textures (R32/RG32/RGBA32 Float) can be sampled
    /// with a linear filter or used by generateMipmaps
    /// (MTLDevice.supports32BitFloatFiltering).
    [[nodiscard]] virtual bool supports_float32_filtering() const {
        return false;
    }
    /// Whether a multisampled 32-bit float render target can be resolved
    /// (Metal Feature Set Tables: Apple9+).
    [[nodiscard]] virtual bool supports_float32_multisample_resolve() const {
        return false;
    }
    /// The largest buffer the device can allocate (MTLDevice.maxBufferLength),
    /// 0 when unknown.
    [[nodiscard]] virtual u64 max_buffer_length() const { return 0; }
    /// Whether a fragment function can read the current value of its colour
    /// attachments (Metal programmable blending, [[color(n)]] inputs: Apple
    /// GPU families only). The D3D11 output-merger logic ops are built on it.
    [[nodiscard]] virtual bool supports_framebuffer_fetch() const {
        return false;
    }
    /// Whether a render pass can place its samples
    /// (MTLRenderPassDescriptor setSamplePositions:count:,
    /// MTLDevice.programmableSamplePositionsSupported).
    [[nodiscard]] virtual bool supports_programmable_sample_positions() const {
        return false;
    }
    /// Whether a fragment shader can export the stencil reference value its
    /// pixel is tested and written with (Metal [[stencil]], MSL 2.1+; SPIR-V
    /// StencilExportEXT / FragStencilRefEXT). D3D11's SV_StencilRef output
    /// (PSSpecifiedStencilRefSupported) is built on it.
    [[nodiscard]] virtual bool supports_shader_stencil_export() const {
        return false;
    }
    /// Whether the CPU and GPU share one physical memory
    /// (MTLDevice.hasUnifiedMemory: Apple silicon yes, a discrete GPU no).
    /// The host fact: the D3D11/DXGI adapter does not report it (it presents
    /// a discrete-class profile, .docs/contracts/dxgi.md "Adapter profile
    /// policy"); the D3D12 frontend's ARCHITECTURE UMA and heap pools do.
    [[nodiscard]] virtual bool has_unified_memory() const { return false; }
    /// Threads the GPU executes in lock step (a SIMD group; D3D's wave lane
    /// count). 0 when unknown.
    [[nodiscard]] virtual u32 simd_group_width() const { return 0; }
    /// Whether push_debug_group / pop_debug_group / insert_debug_signpost
    /// reach the GPU command stream, where GPU capture and profiling tools
    /// show them (Metal debug groups and signposts).
    [[nodiscard]] virtual bool supports_debug_markers() const { return false; }
    /// Open a named group around the GPU work recorded after it, until the
    /// matching pop_debug_group; groups nest. In order with recorded work.
    virtual void push_debug_group(const std::string& /*label*/) {}
    /// Close the innermost open group; with none open, nothing happens.
    virtual void pop_debug_group() {}
    /// A named point between the GPU work recorded before and after it.
    virtual void insert_debug_signpost(const std::string& /*label*/) {}

    // -- Placement heaps ------------------------------------------------------

    /// Whether create_heap can make GPU-private heaps that textures are placed
    /// in (CPU-visible heaps hold buffers only).
    [[nodiscard]] virtual bool supports_placement_heaps() const noexcept {
        return false;
    }
    /// A heap of desc.size bytes, zero-filled. Null when it cannot be made.
    [[nodiscard]] virtual std::unique_ptr<BackendHeap> create_heap(
        const BackendHeapDesc& desc) {
        (void)desc;
        return nullptr;
    }
    /// Bytes and alignment a texture of @p desc takes when placed in a
    /// GPU-private heap. False when it cannot be placed.
    [[nodiscard]] virtual bool heap_texture_placement(
        const BackendTexture2DDesc& desc, u64& size, u64& alignment) {
        (void)desc;
        (void)size;
        (void)alignment;
        return false;
    }
    [[nodiscard]] virtual bool heap_texture_placement(
        const BackendTexture3DDesc& desc, u64& size, u64& alignment) {
        (void)desc;
        (void)size;
        (void)alignment;
        return false;
    }

    // -- Tiled resources ------------------------------------------------------
    /// Whether tiled buffers and 2D textures, tile pools and the mapping and
    /// barrier operations below work.
    [[nodiscard]] virtual bool supports_tiled_resources() const {
        return false;
    }
    /// The 64 KiB tile of a tiled 2D texture of @p format with @p samples
    /// samples, in texels; false when such a texture cannot be tiled.
    [[nodiscard]] virtual bool tiled_texture_tile_shape(
        PixelFormat /*format*/, unsigned /*samples*/, unsigned& /*width*/,
        unsigned& /*height*/) const {
        return false;
    }
    /// A tile pool of no memory yet (BackendTilePool).
    [[nodiscard]] virtual std::unique_ptr<BackendTilePool> create_tile_pool() {
        return nullptr;
    }
    /// The tile layout of a texture created with BackendTexture2DDesc::tiled.
    [[nodiscard]] virtual bool tiled_texture_layout(
        const BackendTexture2D* /*texture*/, BackendTiledLayout& /*layout*/) {
        return false;
    }
    /// Point tiles of a tiled texture or buffer at tiles of @p pool (null when
    /// every entry unmaps), in order with the recorded work: what was recorded
    /// before sees the old mappings, what is recorded after the new ones. The
    /// caller has counted each new pool tile (BackendTilePool::reference).
    /// @p self_aliased: afterwards two of the resource's tiles show one pool
    /// tile, so accesses to different parts of it can touch the same memory
    /// and must stay ordered as accesses to one resource are.
    [[nodiscard]] virtual bool update_tile_mappings(
        BackendTexture2D* /*texture*/, BackendTilePool* /*pool*/,
        const BackendTileMapping* /*mappings*/, usize /*count*/,
        bool /*self_aliased*/) {
        return false;
    }
    [[nodiscard]] virtual bool update_tile_mappings(
        BackendBuffer* /*buffer*/, BackendTilePool* /*pool*/,
        const BackendTileMapping* /*mappings*/, usize /*count*/,
        bool /*self_aliased*/) {
        return false;
    }
    /// Complete every recorded access before any later one starts: the
    /// ordering tiled resources that share pool memory need between them
    /// (ID3D11DeviceContext2::TiledResourceBarrier), which per-resource
    /// hazard tracking cannot see.
    [[nodiscard]] virtual bool tiled_resource_barrier() { return false; }
    /// Copy a texel box of one subresource of a single-sample 2D texture to or
    /// from @p buffer at @p offset, rows @p row_pitch bytes apart (rows of 4x4
    /// blocks for a block-compressed format), in order with the recorded work.
    /// The texels are the texture's native bytes.
    [[nodiscard]] virtual bool copy_texture_buffer_region(
        BackendTexture2D* /*texture*/, unsigned /*level*/, unsigned /*slice*/,
        unsigned /*x*/, unsigned /*y*/, unsigned /*width*/,
        unsigned /*height*/, BackendBuffer* /*buffer*/, u64 /*offset*/,
        u64 /*row_pitch*/, bool /*to_texture*/) {
        return false;
    }
    /// The volume form of copy_texture_buffer_region: a texel box of one mip
    /// of @p texture to or from @p buffer at @p offset, rows @p row_pitch and
    /// W slices @p slice_pitch bytes apart (D3D12 CopyTextureRegion with a
    /// placed footprint of a Texture3D), in order with the recorded work.
    [[nodiscard]] virtual bool copy_texture_3d_buffer_region(
        BackendTexture3D* /*texture*/, unsigned /*level*/, unsigned /*x*/,
        unsigned /*y*/, unsigned /*z*/, unsigned /*width*/,
        unsigned /*height*/, unsigned /*depth*/, BackendBuffer* /*buffer*/,
        u64 /*offset*/, u64 /*row_pitch*/, u64 /*slice_pitch*/,
        bool /*to_texture*/) {
        return false;
    }

    [[nodiscard]] virtual std::string backend_device_name() const = 0;
    [[nodiscard]] virtual void* native_device() const noexcept = 0;
    [[nodiscard]] virtual void* native_command_queue() const noexcept = 0;

    /// Current process allocation and safe resident budget. False means unknown,
    /// never a fabricated zero-usage snapshot. Views must not be double counted.
    [[nodiscard]] virtual bool query_video_memory(BackendVideoMemoryInfo&) const noexcept {
        return false;
    }

    /// Memory the backend recommends keeping resident, in bytes.
    ///
    /// The GPU's working set: DXGI reports it as the adapter's dedicated video
    /// memory and the ceiling of the LOCAL budget (.docs/contracts/dxgi.md
    /// "Adapter profile policy (discrete-class)"). Zero means the backend
    /// does not report one, which callers must treat as "unknown" rather than
    /// "no memory".
    [[nodiscard]] virtual u64 recommended_working_set_size() const noexcept {
        return 0;
    }

    [[nodiscard]] virtual std::unique_ptr<BackendBuffer>
    create_buffer(const BackendBufferDesc& desc,
                  const void* initial_data) = 0;

    [[nodiscard]] virtual std::unique_ptr<BackendTexture2D>
    create_texture_2d(const BackendTexture2DDesc& desc,
                      const void* initial_data,
                      unsigned initial_row_pitch) = 0;

    /// Create a 3D (volume) texture. Default returns null — backends that do
    /// not support volumes fail create honestly at the guest API.
    [[nodiscard]] virtual std::unique_ptr<BackendTexture3D>
    create_texture_3d(const BackendTexture3DDesc& desc,
                      const void* initial_data, unsigned initial_row_pitch,
                      unsigned initial_slice_pitch) {
        (void)desc;
        (void)initial_data;
        (void)initial_row_pitch;
        (void)initial_slice_pitch;
        return nullptr;
    }

    /// Create a texture view over an existing buffer's storage, addressing
    /// `num_elements` texels of `pixel_format` starting `first_element` texels
    /// in. Default returns null — a backend without buffer textures fails the
    /// view creation honestly at the guest API rather than silently reading
    /// the wrong element size. The view does not own the buffer and must not
    /// outlive it.
    ///
    /// `shader_write` asks for a view a shader may also write (a typed buffer
    /// UAV, `RWBuffer<T>`): its writes must land in the buffer's own storage.
    /// Every view aliases that storage (never a copy), so a read observes
    /// every earlier write in stream order; a view whose first element cannot
    /// start a texture reports BackendTextureBuffer::element_bias().
    [[nodiscard]] virtual std::unique_ptr<BackendTextureBuffer>
    create_texture_buffer(const BackendBuffer& buffer, PixelFormat pixel_format,
                          unsigned first_element, unsigned num_elements,
                          bool shader_write = false) {
        (void)buffer;
        (void)pixel_format;
        (void)first_element;
        (void)num_elements;
        (void)shader_write;
        return nullptr;
    }

    /// A read-only typed buffer view whose components are remapped by
    /// @p component_mapping (BackendTextureViewDesc::component_mapping's
    /// encoding; 0 is the identity). Null when the backend cannot remap.
    [[nodiscard]] virtual std::unique_ptr<BackendTextureBuffer>
    create_mapped_texture_buffer(const BackendBuffer& buffer,
                                 PixelFormat pixel_format,
                                 unsigned first_element, unsigned num_elements,
                                 u16 component_mapping) {
        if (component_mapping != 0) return nullptr;
        return create_texture_buffer(buffer, pixel_format, first_element,
                                     num_elements, false);
    }

    [[nodiscard]] virtual std::unique_ptr<BackendTexture2D>
    open_shared_texture_2d(u64 handle) {
        (void)handle;
        return nullptr;
    }

    /// Whether a shareable 2D texture (single sample, mip and slice) of
    /// @p format has a representation another device or process can open.
    [[nodiscard]] virtual bool supports_shared_texture_2d(
        PixelFormat format) const noexcept {
        (void)format;
        return false;
    }

    /// Keep the storage behind @p handle alive for as long as the returned
    /// reference is held (an NT shared-resource handle holds one: Learn,
    /// IDXGIResource1::CreateSharedHandle). Null when the handle names no
    /// live shared storage.
    [[nodiscard]] virtual std::shared_ptr<void> retain_shared_texture(
        u64 handle) {
        (void)handle;
        return nullptr;
    }

    /// Whether a texture of @p format can be shared in any shape (mirrored
    /// through a canonical store): every format the backend can create and
    /// copy to and from memory. Stricter than supports_shared_texture_2d,
    /// which is the zero-copy single-image form.
    [[nodiscard]] virtual bool supports_shared_texture(
        PixelFormat format) const noexcept {
        return supports_shared_texture_2d(format);
    }

    /// Whether every resource kind can be shared (buffers, volumes, mipmapped,
    /// array, cube and multisampled textures, shared CPU memory): the
    /// backend half of D3D11 ExtendedResourceSharing.
    [[nodiscard]] virtual bool supports_extended_sharing() const noexcept {
        return false;
    }

    /// Shared CPU memory of @p bytes with @p metadata stored alongside
    /// (BackendSharedMemory). Null when unsupported.
    [[nodiscard]] virtual std::shared_ptr<BackendSharedMemory>
    create_shared_memory(usize bytes, std::vector<u8> metadata) {
        (void)bytes;
        (void)metadata;
        return nullptr;
    }

    /// What open_shared_resource found behind a shared id.
    struct SharedOpening {
        enum class Kind : u8 { None, Texture2D, Texture3D, Buffer, Memory };
        Kind kind{Kind::None};
        /// The creator's shared_metadata.
        std::vector<u8> metadata;
        std::unique_ptr<BackendTexture2D> texture_2d;
        std::unique_ptr<BackendTexture3D> texture_3d;
        std::unique_ptr<BackendBuffer> buffer;
        std::shared_ptr<BackendSharedMemory> memory;
    };

    /// Open the shared resource @p handle names -- created by this process or
    /// another one -- as a new object over the same storage. An opening in
    /// the process that holds the resource shares its storage outright; one
    /// in another process mirrors the canonical store. Kind::None when the
    /// id names nothing shared.
    [[nodiscard]] virtual SharedOpening open_shared_resource(u64 handle) {
        SharedOpening opening;
        opening.texture_2d = open_shared_texture_2d(handle);
        if (opening.texture_2d) {
            opening.kind = SharedOpening::Kind::Texture2D;
            opening.metadata = opening.texture_2d->shared_metadata();
        }
        return opening;
    }

    /// A synchronization point of the producer (D3D11 Flush, Present,
    /// IDXGIKeyedMutex::ReleaseSync; Learn OpenSharedResource: "If a shared
    /// texture is updated on one device ID3D11DeviceContext::Flush must be
    /// called on that device"): submit what this process wrote into shared
    /// resources other processes can open and publish it once the GPU has
    /// completed it. Returns the completion of that work (null: nothing was
    /// published, or the backend has no completion tracking).
    [[nodiscard]] virtual std::shared_ptr<GpuCompletion>
    publish_shared_writes() {
        return nullptr;
    }

    /// A synchronization point of the consumer (IDXGIKeyedMutex::AcquireSync,
    /// an opening): wait for the writes other processes published to shared
    /// resources this process uses and order them before the work recorded
    /// from now on. Backends also do this before every recorded operation.
    virtual void acquire_shared_writes() {}

    /// GPU copies between a texture subresource box and shared CPU memory
    /// (a shared STAGING resource), in order with the recorded work: rows of
    /// @p row_pitch bytes and W slices of @p image_pitch bytes starting at
    /// @p offset. False when unsupported.
    [[nodiscard]] virtual bool copy_texture_to_shared_memory(
        const BackendTextureLocation& src, unsigned width, unsigned height,
        unsigned depth, BackendSharedMemory& dst, usize offset,
        usize row_pitch, usize image_pitch) {
        (void)src;
        (void)width;
        (void)height;
        (void)depth;
        (void)dst;
        (void)offset;
        (void)row_pitch;
        (void)image_pitch;
        return false;
    }
    [[nodiscard]] virtual bool copy_shared_memory_to_texture(
        BackendSharedMemory& src, usize offset, usize row_pitch,
        usize image_pitch, const BackendTextureLocation& dst, unsigned width,
        unsigned height, unsigned depth) {
        (void)src;
        (void)offset;
        (void)row_pitch;
        (void)image_pitch;
        (void)dst;
        (void)width;
        (void)height;
        (void)depth;
        return false;
    }

    /// Whether this backend can record command lists.
    [[nodiscard]] virtual bool supports_command_lists() const noexcept {
        return false;
    }

    /// Create an open, empty command list.
    [[nodiscard]] virtual std::unique_ptr<BackendCommandList>
    create_command_list() {
        return nullptr;
    }

    /// Append @p list's operations to the immediate stream, so they execute in
    /// the order they were recorded, after everything already recorded here.
    ///
    /// The list stays intact and replayable: every resource it references is
    /// retained again, because D3D11 permits one command list to be executed more
    /// than once.
    [[nodiscard]] virtual bool submit_command_list(BackendCommandList* list) {
        (void)list;
        return false;
    }

    /// Whether this backend can serve timeline fences. A backend that cannot
    /// returns false and create_fence returns null, so a frontend fails
    /// `CreateFence` honestly instead of handing back an object whose waits
    /// never complete.
    [[nodiscard]] virtual bool supports_fences() const noexcept { return false; }

    /// Create a timeline fence starting at @p initial_value.
    [[nodiscard]] virtual std::unique_ptr<BackendFence> create_fence(
        u64 initial_value) {
        (void)initial_value;
        return nullptr;
    }

    /// Whether this backend can serve descriptor tables. A backend that cannot
    /// returns false and create_descriptor_table returns null, so a frontend
    /// falls back to the flat path and its narrower register counts rather than
    /// binding into nothing.
    [[nodiscard]] virtual bool supports_descriptor_tables() const noexcept {
        return false;
    }

    /// native-state (R3c): a sampler / depth-stencil state built once.
    /// Null (the default) when the backend has none; draws then keep naming
    /// states by their descs.
    [[nodiscard]] virtual std::unique_ptr<BackendSamplerState>
    create_sampler_state(const BackendSamplerDesc& desc) {
        (void)desc;
        return nullptr;
    }
    [[nodiscard]] virtual std::unique_ptr<BackendDepthStencilState>
    create_depth_stencil_state(const BackendDepthStencilDesc& desc) {
        (void)desc;
        return nullptr;
    }

    /// Create a descriptor table. Null when unsupported or the descriptor is
    /// not serviceable.
    [[nodiscard]] virtual std::unique_ptr<BackendDescriptorTable>
    create_descriptor_table(const BackendDescriptorTableDesc& desc) {
        (void)desc;
        return nullptr;
    }

    [[nodiscard]] virtual std::unique_ptr<BackendSwapChain>
    create_swap_chain(const BackendSwapChainDesc& desc) = 0;

    /// Compile a pipeline from MSL and a vertex layout. Repeated calls with
    /// the same `cache_key` should return an object backed by one compilation.
    [[nodiscard]] virtual std::unique_ptr<BackendPipeline>
    create_pipeline(const BackendPipelineDesc& desc) = 0;

    /// A hint that a shader with this source will be part of a pipeline soon:
    /// a backend may start compiling it in the background, so the draw that
    /// first needs it does not wait for the whole compile. D3D11 forms a
    /// pipeline only at draw time, but knows each shader when the application
    /// creates it -- usually well before, on its own loading threads. Never
    /// required for correctness; a backend that ignores it compiles at
    /// create_pipeline as before.
    virtual void prefetch_shader(const std::string& msl,
                                 const std::string& entry_point,
                                 BackendMathMode math_mode,
                                 bool preserve_invariance) {
        (void)msl;
        (void)entry_point;
        (void)math_mode;
        (void)preserve_invariance;
    }

    /// Compile a compute pipeline from MSL. Repeated calls with the same
    /// `cache_key` should return an object backed by one compilation.
    [[nodiscard]] virtual std::unique_ptr<BackendComputePipeline>
    create_compute_pipeline(const BackendComputePipelineDesc& desc) = 0;

    /// Create a render pass targeting explicit MRT + depth textures. Not tied
    /// to a swapchain; caller manages target lifetimes. Returns null if any
    /// color target lacks render_target=true or depth lacks depth_stencil=true.
    [[nodiscard]] virtual std::unique_ptr<BackendRenderPass>
    create_render_pass_standalone(const BackendRenderPassDesc& desc) = 0;

    /// Submit a compute dispatch. Uses an internal command buffer; for async
    /// execution alongside rendering, frontends should manage command buffer
    /// overlap explicitly.
    [[nodiscard]] virtual bool dispatch(const BackendDispatchCall& call) = 0;

    /// Full-resource buffer copy (same size). Returns false when sizes differ
    /// or either buffer is invalid.
    [[nodiscard]] virtual bool copy_buffer(BackendBuffer* dst,
                                           const BackendBuffer* src) = 0;

    /// Copy @p bytes from @p src at @p src_offset into @p dst at
    /// @p dst_offset.
    ///
    /// The sub-range form that `copy_buffer` is not: D3D11
    /// `CopySubresourceRegion` on a buffer, D3D11 `CopyStructureCount` (four
    /// bytes out of a UAV counter), and D3D12 `CopyBufferRegion` all need it.
    /// Returns false when either range runs past the end of its buffer, when
    /// @p bytes is zero, or when the two ranges overlap within one buffer --
    /// the result of an overlapping blit is undefined, so it is refused rather
    /// than silently producing one of the possible answers.
    ///
    /// A backend that does not implement it returns false, and a frontend must
    /// then fail its call rather than leave the destination unwritten.
    [[nodiscard]] virtual bool copy_buffer_region(BackendBuffer* dst,
                                                  u64 dst_offset,
                                                  const BackendBuffer* src,
                                                  u64 src_offset, u64 bytes) {
        (void)dst;
        (void)dst_offset;
        (void)src;
        (void)src_offset;
        (void)bytes;
        return false;
    }

    /// Set every byte of [@p offset, @p offset + @p bytes) of @p dst to
    /// @p value, in order with the recorded work: a clear that needs no
    /// source buffer. Returns false when the range runs past the end, when
    /// @p bytes is zero, or when @p offset or @p bytes is not a multiple of
    /// four (the granularity this contract promises).
    ///
    /// A backend that does not implement it returns false; a frontend then
    /// writes the range another way (a copy from a pattern buffer).
    [[nodiscard]] virtual bool fill_buffer_region(BackendBuffer* dst,
                                                  u64 offset, u64 bytes,
                                                  u8 value) {
        (void)dst;
        (void)offset;
        (void)bytes;
        (void)value;
        return false;
    }

    /// Copy @p width texels of row 0, mip 0, slice 0 of a single-sample
    /// @p texture between it and @p bytes of @p buffer at @p offset (texels
    /// packed; the offset a multiple of the texel size), in order with the
    /// recorded work: into the texture with @p to_texture, else out of it.
    /// What a render-target view of a buffer draws through (D3D11
    /// D3D11_RTV_DIMENSION_BUFFER). Default: unsupported.
    [[nodiscard]] virtual bool copy_buffer_texture_row(
        BackendBuffer* buffer, u64 offset, BackendTexture2D* texture,
        unsigned width, bool to_texture) {
        (void)buffer;
        (void)offset;
        (void)texture;
        (void)width;
        (void)to_texture;
        return false;
    }

    /// Full-resource 2D texture copy (same size/format/mips). Returns false
    /// when descriptors are incompatible.
    [[nodiscard]] virtual bool copy_texture_2d(BackendTexture2D* dst,
                                               const BackendTexture2D* src) = 0;

    /// Resolve one multisampled 2D texture subresource into a matching
    /// single-sample subresource. Returns false when the selected mip/slice
    /// is invalid or the formats, dimensions, or sample counts disagree.
    [[nodiscard]] virtual bool resolve_texture_2d(
        BackendTexture2D* dst, unsigned dst_mip, unsigned dst_slice,
        const BackendTexture2D* src, unsigned src_mip,
        unsigned src_slice, PixelFormat resolve_format) {
        (void)dst;
        (void)dst_mip;
        (void)dst_slice;
        (void)src;
        (void)src_mip;
        (void)src_slice;
        (void)resolve_format;
        return false;
    }

    /// Same-format 2D subresource rectangle copy. Returns false when the
    /// selected mip/slice or rectangles are out of bounds, formats differ,
    /// or the backend cannot blit the region.
    [[nodiscard]] virtual bool copy_texture_2d_region(
        BackendTexture2D* dst, unsigned dst_x, unsigned dst_y,
        const BackendTexture2D* src, unsigned src_x, unsigned src_y,
        unsigned width, unsigned height, unsigned dst_mip = 0,
        unsigned dst_slice = 0, unsigned src_mip = 0,
        unsigned src_slice = 0) {
        (void)dst;
        (void)dst_x;
        (void)dst_y;
        (void)src;
        (void)src_x;
        (void)src_y;
        (void)width;
        (void)height;
        (void)dst_mip;
        (void)dst_slice;
        (void)src_mip;
        (void)src_slice;
        return false;
    }

    /// Byte-exact copy of a box between two textures whose texels -- or, for
    /// a block-compressed side, whose 4x4 blocks -- have the same size, in
    /// order with the recorded work. The pixel formats may differ: D3D
    /// copies "reinterpret the bits" between members of a format family, and
    /// D3D10.1 copies a BC texture's blocks to and from an uncompressed one
    /// whose texel is one block (Learn, "Format Conversion using Direct3D
    /// 10.1"). `width`/`height`/`depth` are in the source's texels, which for
    /// a compressed source is a whole number of blocks or reaches the level's
    /// edge; the destination box is that many elements in its own units. The
    /// two may be the same texture, even the same subresource with
    /// overlapping boxes: the result is as if the source were read whole
    /// before anything is written (D3D11.3 5.6.2.1). Default: unsupported.
    [[nodiscard]] virtual bool copy_texture_elements(
        const BackendTextureLocation& dst, const BackendTextureLocation& src,
        unsigned width, unsigned height, unsigned depth) {
        (void)dst;
        (void)src;
        (void)width;
        (void)height;
        (void)depth;
        return false;
    }

    /// Scale a same-format 2D sub-rectangle (level 0) from `src` into `dst`.
    /// `filter` is nearest (D3DTEXF_NONE / POINT) or linear (D3DTEXF_LINEAR).
    /// `dst_slice` / `src_slice` select a cube face or 2D-array slice.
    /// Returns false when the rectangles are empty or out of bounds, formats
    /// differ, dest is not a render target, or the backend cannot stretch.
    [[nodiscard]] virtual bool stretch_texture_2d_region(
        BackendTexture2D* dst, unsigned dst_x, unsigned dst_y,
        unsigned dst_width, unsigned dst_height, const BackendTexture2D* src,
        unsigned src_x, unsigned src_y, unsigned src_width,
        unsigned src_height, BackendFilter filter, unsigned dst_slice = 0,
        unsigned src_slice = 0) {
        (void)dst;
        (void)dst_x;
        (void)dst_y;
        (void)dst_width;
        (void)dst_height;
        (void)src;
        (void)src_x;
        (void)src_y;
        (void)src_width;
        (void)src_height;
        (void)filter;
        (void)dst_slice;
        (void)src_slice;
        return false;
    }

    /// Generate remaining mips from level 0. Returns false when the texture
    /// has a single mip or the format cannot be filtered.
    [[nodiscard]] virtual bool generate_mips(BackendTexture2D* texture) = 0;

    /// Regenerate only a window of the chain: levels `first_mip + 1` ..
    /// `first_mip + mip_count - 1` of array slices `first_slice` ..
    /// `first_slice + slice_count - 1`, each from the level above it
    /// (Learn, ID3D11DeviceContext::GenerateMips on a view). The caller has
    /// already checked the format is filterable. Default: only the whole
    /// texture, through generate_mips.
    [[nodiscard]] virtual bool generate_mips_window(
        BackendTexture2D* texture, unsigned first_mip, unsigned mip_count,
        unsigned first_slice, unsigned slice_count) {
        if (texture == nullptr || first_mip != 0 || first_slice != 0 ||
            mip_count != texture->desc().mip_levels ||
            slice_count != texture->desc().array_size) {
            return false;
        }
        return generate_mips(texture);
    }

    /// Generate remaining 3D mips from level 0. Default returns false —
    /// backends without volume mip generation fail honestly at the guest API.
    /// Full-resource volume copy (same size/format/mips), ordered with the
    /// recorded draws and dispatches like copy_texture_2d. Default returns
    /// false so a backend without it fails CopyResource visibly.
    [[nodiscard]] virtual bool copy_texture_3d(BackendTexture3D* dst,
                                               const BackendTexture3D* src) {
        (void)dst;
        (void)src;
        return false;
    }

    [[nodiscard]] virtual bool generate_mips(BackendTexture3D* texture) {
        (void)texture;
        return false;
    }

    /// D3D9 SetGammaRamp / GDI SetDeviceGammaRamp: 256 WORD samples per
    /// channel (R then G then B). Null or identity disables the present LUT.
    /// Learn: the ramp maps framebuffer levels to display levels immediately.
    virtual void set_present_gamma_ramp(const u16* rgb_256x3) {
        (void)rgb_256x3;
    }

    /// Hand every recorded command to the GPU. Learn
    /// (ID3D11DeviceContext::Flush, and GetData without
    /// D3D11_ASYNC_GETDATA_DONOTFLUSH) requires only the submission; a
    /// backend MAY also wait for completion (Metal currently does, see
    /// MetalDevice::flush). A caller that must observe completion uses
    /// wait_idle() / wait_generation(), never flush().
    virtual void flush() = 0;

    /// Whether the GPU is about to run out of submitted work (at most one
    /// submission still unfinished), so that submitting what has been
    /// recorded so far, instead of waiting for the frame's Present, would let
    /// it start sooner. A driver may submit at any time (Learn,
    /// ID3D11DeviceContext::Flush: the call only makes it happen now). A
    /// backend without a submission queue returns false: never ask early.
    [[nodiscard]] virtual bool wants_early_submit() const { return false; }

    /// Submit encoded GPU work and return a completion that waits only for
    /// that submission. Later encodes on the same queue must not block this
    /// wait — IDXGISwapChain::Present (SyncInterval 0) publishes a specific
    /// frame, not the whole queue. Default flushes and returns null so the
    /// caller can fall back to wait_idle().
    [[nodiscard]] virtual std::shared_ptr<GpuCompletion> flush_completion() {
        flush();
        return nullptr;
    }

    /// Submission generation that work recorded on the immediate stream from
    /// now on belongs to. Generations are monotonic and complete in order.
    /// Zero means "not tracked" -- the backend has no generations, or this
    /// thread is capturing into a command list whose work runs at some later
    /// execute -- and callers must fall back to wait_idle().
    [[nodiscard]] virtual u64 recording_generation() { return 0; }

    /// The submission generation the calling thread's most recent operation
    /// on the immediate stream was recorded into, taken by the backend as the
    /// operation joined the recording. Never earlier than that operation's
    /// own -- unlike a recording_generation() read before recording, which
    /// another thread's submit can make one too early -- and always one that
    /// is submitted when the recording is flushed. Read it right after
    /// recording the work a completion must cover (a query's statistics
    /// record, a stream-output record). Zero means "not tracked", as for
    /// recording_generation(): the operation was captured into a command
    /// list, or the backend has no generations.
    [[nodiscard]] virtual u64 last_recorded_generation() { return 0; }

    /// Draws the backend's encoder met with their render pass's encoder not
    /// open: another thread's submit (or recorded operation) came between
    /// the pass's begin and the draw. `reopened` it placed by beginning the
    /// pass again, loading what the pass stored so far; `dropped` it could
    /// not place at all (no such pass open, or no encoder). Monotonic, for
    /// diagnostics; zero for a backend that never splits a pass.
    [[nodiscard]] virtual BackendDrawEncodeCounts draw_encode_counts() const {
        return {};
    }

    /// True once the GPU has finished every submission up to and including
    /// @p generation. Never true for an untracked (zero) generation.
    [[nodiscard]] virtual bool generation_complete(u64 generation) {
        (void)generation;
        return false;
    }

    /// Submit the recording if it still holds @p generation, then wait until
    /// that generation -- and nothing recorded after it -- has completed.
    /// This is the per-resource wait of ID3D11DeviceContext::Map: a resource
    /// the GPU last touched in generation G is CPU-safe once G retires, and
    /// the rest of the frame must not extend the wait.
    [[nodiscard]] virtual bool wait_generation(u64 generation) {
        (void)generation;
        return wait_idle();
    }

    /// Record a GpuTimestamp at this point of the command stream (or of the
    /// command list this thread is capturing). Recording does not submit;
    /// flush() does. Null means the backend cannot time GPU work.
    [[nodiscard]] virtual std::shared_ptr<GpuTimestamp> record_timestamp() {
        return nullptr;
    }

    /// A new, zeroed GpuOcclusionCounter for render passes recorded on the
    /// immediate stream. Null means the backend cannot count samples; a caller
    /// must then fail its query rather than report a count.
    [[nodiscard]] virtual std::shared_ptr<GpuOcclusionCounter>
    create_occlusion_counter() {
        return nullptr;
    }

    /// Submit and wait for all previously encoded GPU work.
    ///
    /// Cross-process shared textures must not be published until writes are
    /// complete because command-queue ordering does not cross process
    /// boundaries. Backends without an explicit completion primitive return
    /// false so callers can use a synchronized fallback.
    [[nodiscard]] virtual bool wait_idle() {
        flush();
        return false;
    }
};

// -----------------------------------------------------------------------------
// Active backend
// -----------------------------------------------------------------------------
//
// Windows exposes adapter identity through APIs that have nothing to do with
// rendering: DXGI enumerates adapters, GDI and SetupAPI report a display
// device name, user32 describes the primary monitor. Those frontends need to
// know *which* adapter is present without knowing what kind it is.
//
// The composition root installs the backend it built; frontends ask here. A
// frontend that reaches for a concrete backend instead — MetalDevice, say —
// cannot be built or tested without that backend, and would have to be
// rewritten for a second one.
//
// Null until installed, which is the correct state for a headless unit test:
// callers already handle an unavailable adapter, and must treat null the same
// way.

/// The graphics backend in use, or nullptr if none has been installed.
[[nodiscard]] GraphicsBackendDevice* active_backend_device() noexcept;

/// Install the active backend. Called once by the composition root during
/// startup; passing nullptr clears it. The device must outlive the call to
/// clear it — ownership stays with the caller.
void set_active_backend_device(GraphicsBackendDevice* device) noexcept;

} // namespace vitreo::graphics
