// SPDX-License-Identifier: LGPL-2.1-only
/// @file metal_buffer_pool.mm
/// @brief Shared slab storage for small constant buffers.

#import <Metal/Metal.h>
#include "graphics/metal/metal_buffer_pool.hpp"
#include "graphics/metal/metal_submit_queue.hpp"

namespace vitreo::graphics {

MetalConstantBufferPool& MetalConstantBufferPool::instance() {
    // Never destroyed: a MetalBuffer released during static teardown still
    // returns its slot here.
    static MetalConstantBufferPool* pool = new MetalConstantBufferPool();
    return *pool;
}

bool MetalConstantBufferPool::acquire(void* device, usize bytes,
                                      PooledSlot& out) {
    if (device == nullptr || bytes == 0 || bytes > kMaxBytes) return false;
    u32 size_class = 0;
    while ((kMinBytes << size_class) < bytes) ++size_class;
    const usize slot_bytes = kMinBytes << size_class;
    // Read outside the lock: it only grows, so a stale value only defers a
    // reuse, never brings one forward.
    const u64 done = MetalSubmitQueue::instance().completed_generation();

    std::lock_guard<std::mutex> lock(m_mu);
    if (m_device == nullptr) {
        m_device = device;
    } else if (m_device != device) {
        return false;
    }
    SizeClass& cls = m_classes[size_class];
    // Releases arrive in generation order up to a race between threads, so
    // a front entry that has not retired only delays the ones behind it.
    while (!cls.retiring.empty() && cls.retiring.front().generation <= done) {
        cls.free.push_back(cls.retiring.front().slot);
        cls.retiring.pop_front();
    }
    if (!cls.free.empty()) {
        out = cls.free.back();
        cls.free.pop_back();
        return true;
    }
    if (!cls.carving || cls.carve_next + slot_bytes > kSlabBytes) {
        id<MTLDevice> mtl = (__bridge id<MTLDevice>)device;
        id<MTLBuffer> slab =
            [mtl newBufferWithLength:kSlabBytes
                             options:MTLResourceStorageModeShared];
        if (slab == nil || [slab contents] == nullptr) return false;
        m_slabs.push_back((__bridge_retained void*)slab);
        cls.carving = true;
        cls.carve_slab = static_cast<u32>(m_slabs.size() - 1);
        cls.carve_next = 0;
    }
    void* slab = m_slabs[cls.carve_slab];
    out.buffer = slab;
    out.offset = cls.carve_next;
    out.contents =
        static_cast<u8*>([(__bridge id<MTLBuffer>)slab contents]) +
        cls.carve_next;
    out.size_class = size_class;
    out.index = cls.carve_slab;
    cls.carve_next += slot_bytes;
    return true;
}

void MetalConstantBufferPool::release(const PooledSlot& slot,
                                      u64 retire_generation) {
    if (slot.buffer == nullptr || slot.size_class >= kClasses) return;
    std::lock_guard<std::mutex> lock(m_mu);
    m_classes[slot.size_class].retiring.push_back(
        Retiring{slot, retire_generation});
}

} // namespace vitreo::graphics
