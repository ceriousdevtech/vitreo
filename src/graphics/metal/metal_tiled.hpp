// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_tiled.hpp
/// @brief D3D11.2 tiled resources on Metal 4 placement sparse resources.
///
/// A tiled texture or buffer is a placement sparse MTLTexture / MTLBuffer
/// with a 64 KiB sparse page (MTLSparsePageSize64), which on Apple GPUs has
/// exactly the D3D standard tile shape for every single-sample and 4x format
/// checked (tiled_texture_tile_shape compares them per format). A tile pool
/// is placement heaps (MTLHeapTypePlacement) allocated in granules as its
/// tiles are first mapped. Mapping updates are MTL4CommandQueue
/// updateTextureMappings / updateBufferMappings, ordered against the legacy
/// command queue with a shared event: the command buffer before an update
/// signals, the Metal 4 queue waits for it, updates, and signals the value the
/// next command buffer waits for. Every granule heap sits in one residency
/// set attached to both queues -- placement sparse memory is not made
/// resident by binding the resource that maps it (measured: without the set,
/// reads through a mapped tile return garbage).

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"

#include <atomic>
#include <memory>

namespace vitreo::graphics {

/// A resolved mapping update, ready for the Metal 4 queue (metal_tiled.mm).
struct MetalTileMappingBatch;

/// Record a tile-mapping update in the submit queue's stream
/// (metal_submit_queue_record_ops.mm). Declared here so metal_tiled.mm can
/// reach it without the queue's private Op.
void metal_record_tile_mapping(std::shared_ptr<MetalTileMappingBatch> batch);

class MetalTiled {
public:
    static MetalTiled& instance();

    /// Placement sparse resources and the Metal 4 queue exist (macOS 26 on a
    /// GPU of the Metal 4 family). Initialises on first call.
    [[nodiscard]] bool available();

    /// The 64 KiB tile of a 2D (samples 1) or 2D multisample texture of
    /// @p mtl_pixel_format, in texels, when such a texture can be created
    /// placement sparse.
    [[nodiscard]] bool tile_shape(u32 mtl_pixel_format, unsigned samples,
                                  unsigned& width, unsigned& height);

    /// A placement sparse buffer of @p bytes (rounded up to whole tiles):
    /// a retained id<MTLBuffer>, or null.
    [[nodiscard]] void* new_sparse_buffer(u64 bytes);

    /// firstMipmapInTail and tailSizeInBytes of a placement sparse texture.
    [[nodiscard]] bool texture_tail(void* texture, unsigned& first_packed_level,
                                    unsigned& packed_tiles);

    [[nodiscard]] std::unique_ptr<BackendTilePool> create_pool();

    /// CopyTiles / UpdateTiles of a box of a multisampled texture (native
    /// handle) against a buffer at @p buffer_offset (native) + @p offset,
    /// in the D3D linear layout: every pixel's samples in index order,
    /// rows @p row_pitch bytes apart; depth as its value (D16 as the 16-bit
    /// code). Recorded in order with the work around it.
    [[nodiscard]] bool copy_multisampled(void* texture, unsigned slice,
                                         u32 texel_bytes,
                                         bool depth16, unsigned x,
                                         unsigned y, unsigned width,
                                         unsigned height, void* buffer,
                                         usize buffer_offset, u64 offset,
                                         u64 row_pitch, bool to_texture);

    /// Resolve @p count mappings of @p resource (an id<MTLTexture> when
    /// @p texture, else an id<MTLBuffer>) into a batch and record it.
    /// @p first_packed_level addresses a texture's packed tail.
    [[nodiscard]] bool record_update(void* resource, bool texture,
                                     unsigned first_packed_level,
                                     BackendTilePool* pool,
                                     const BackendTileMapping* mappings,
                                     usize count, bool self_aliased);

    /// Whether @p resource (a native handle) has two tiles on one pool tile.
    /// Metal orders blits by the byte ranges or regions they touch, not by
    /// memory, so the submit queue fences blits on such a resource (measured:
    /// a write through one page and a read through its alias in the next
    /// encoder read zeros without a barrier).
    [[nodiscard]] bool self_aliased(const void* resource);
    /// @p resource is being destroyed: drop what is kept about it.
    void forget(const void* resource);

    /// Submit-queue worker: run @p batch on the Metal 4 queue after the
    /// shared event reaches @p wait_value, then signal @p signal_value. The
    /// batch is kept until GPU generation @p gpu_gen retires.
    void encode(const std::shared_ptr<MetalTileMappingBatch>& batch,
                u64 wait_value, u64 signal_value, u64 gpu_gen);

    /// The id<MTLSharedEvent> the queues order on, and the next two values
    /// for one update (called only by the submit-queue worker).
    [[nodiscard]] void* event() const noexcept { return m_event; }
    [[nodiscard]] u64 take_event_values() noexcept {
        m_event_value += 2;
        return m_event_value - 1;
    }

    /// Hold @p object (retained, CFRelease'd later) until GPU generation
    /// @p gpu_gen retires. Also drops what earlier calls parked.
    void release_after(void* object, u64 gpu_gen, bool resident);
    void collect();

    MetalTiled(const MetalTiled&) = delete;
    MetalTiled& operator=(const MetalTiled&) = delete;

private:
    MetalTiled() = default;
    void init();

    friend class MetalTilePool;
    /// Add a granule heap to the residency set (the set is committed here).
    void make_resident(void* heap);

    std::atomic<u32> m_self_aliased_count{0};
    bool m_initialised{false};
    bool m_available{false};
    void* m_device{nullptr};    ///< id<MTLDevice>
    void* m_queue4{nullptr};    ///< id<MTL4CommandQueue>, retained
    void* m_event{nullptr};     ///< id<MTLSharedEvent>, retained
    void* m_residency{nullptr}; ///< id<MTLResidencySet>, retained
    u64 m_event_value{0};
};

}  // namespace vitreo::graphics
