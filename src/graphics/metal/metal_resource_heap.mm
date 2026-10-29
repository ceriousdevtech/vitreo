/// @file metal_resource_heap.mm
/// @brief Staging ring-buffer and deferred resource destruction implementation

#import <Metal/Metal.h>
#include "graphics/metal/metal_resource_heap.hpp"
#include <algorithm>

namespace vitreo::graphics {

static constexpr usize kStagingCapacity = 16 * 1024 * 1024;  // 16 MB

MetalResourceHeap::MetalResourceHeap(void* device)
    : m_device(device), m_staging_capacity(kStagingCapacity)
{
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
    id<MTLBuffer> buf = [mtl_device newBufferWithLength:kStagingCapacity
                                                options:MTLResourceStorageModeShared];
    m_staging_buffer = (__bridge_retained void*)buf;
    (void)m_device; // retained for future private-storage uploads

    VITREO_TRACE("[heap] created staging buffer %zu bytes\n", kStagingCapacity);
}

MetalResourceHeap::~MetalResourceHeap() {
    // Release all deferred resources immediately.
    for (auto& dr : m_deferred_buffers) {
        if (dr.resource) CFRelease(dr.resource);
    }
    for (auto& dr : m_deferred_textures) {
        if (dr.resource) CFRelease(dr.resource);
    }
    m_deferred_buffers.clear();
    m_deferred_textures.clear();

    if (m_staging_buffer) {
        CFRelease(m_staging_buffer);
        m_staging_buffer = nullptr;
    }
}

MetalResourceHeap::StagingAlloc
MetalResourceHeap::alloc_staging(usize size, usize alignment) {
    // Align offset up.
    usize aligned = (m_staging_offset + alignment - 1) & ~(alignment - 1);
    if (aligned + size > m_staging_capacity) {
        VITREO_TRACE("[heap] staging exhausted (need %zu, have %zu)\n",
                     aligned + size, m_staging_capacity);
        return {nullptr, 0, nullptr};
    }

    id<MTLBuffer> buf = (__bridge id<MTLBuffer>)m_staging_buffer;
    void* cpu = static_cast<u8*>([buf contents]) + aligned;

    StagingAlloc alloc{m_staging_buffer, aligned, cpu};
    m_staging_offset = aligned + size;
    return alloc;
}

void MetalResourceHeap::reset_staging_for_frame(u64 /*completed_frame*/) {
    m_staging_offset = 0;
}

void MetalResourceHeap::defer_destroy_buffer(void* buffer, u64 frame) {
    // Retain so the resource stays alive until we collect.
    CFRetain(buffer);
    m_deferred_buffers.push_back({buffer, frame});
}

void MetalResourceHeap::defer_destroy_texture(void* texture, u64 frame) {
    CFRetain(texture);
    m_deferred_textures.push_back({texture, frame});
}

void MetalResourceHeap::collect_garbage(u64 completed_frame) {
    auto release_old = [&](std::vector<DeferredResource>& vec) {
        auto it = std::remove_if(vec.begin(), vec.end(),
            [&](const DeferredResource& dr) {
                if (dr.frame <= completed_frame) {
                    CFRelease(dr.resource);
                    return true;
                }
                return false;
            });
        vec.erase(it, vec.end());
    };

    release_old(m_deferred_buffers);
    release_old(m_deferred_textures);
}

} // namespace vitreo::graphics
