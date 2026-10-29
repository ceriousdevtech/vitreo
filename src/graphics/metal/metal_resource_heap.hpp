// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_resource_heap.hpp
/// @brief Staging ring-buffer and deferred resource destruction

#include <vitreo/common.hpp>
#include <vector>

namespace vitreo::graphics {

class MetalResourceHeap {
public:
    explicit MetalResourceHeap(void* device);  // id<MTLDevice>
    ~MetalResourceHeap();

    struct StagingAlloc {
        void* buffer;   // id<MTLBuffer> (the whole staging buffer)
        usize offset;
        void* cpu_ptr;
    };

    StagingAlloc alloc_staging(usize size, usize alignment = 256);
    void reset_staging_for_frame(u64 completed_frame);

    void defer_destroy_buffer(void* buffer, u64 frame);
    void defer_destroy_texture(void* texture, u64 frame);
    void collect_garbage(u64 completed_frame);

    MetalResourceHeap(const MetalResourceHeap&) = delete;
    MetalResourceHeap& operator=(const MetalResourceHeap&) = delete;

private:
    void* m_device = nullptr;
    void* m_staging_buffer = nullptr;  // id<MTLBuffer> shared
    usize m_staging_capacity = 0;
    usize m_staging_offset = 0;

    struct DeferredResource { void* resource; u64 frame; };
    std::vector<DeferredResource> m_deferred_buffers;
    std::vector<DeferredResource> m_deferred_textures;
};

} // namespace vitreo::graphics
