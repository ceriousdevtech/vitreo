// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_backend_buffer.mm
/// @brief D3D12 heaps and the buffer-side MetalDevice members

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#include "metal_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_gpu_profile.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"
#include <vitreo/ab_cycle.hpp>

#include <atomic>
#include <cmath>
#include <chrono>
#include "graphics/metal/metal_texture.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "core/memory/host_memory_floor.hpp"

#include <mach/mach.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <limits>
#include <memory>
#include <map>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <thread>
#include <deque>
#include <unistd.h>
#include <condition_variable>
#include <vector>

#include "graphics/metal/metal_backend_buffer.hpp"

using namespace vitreo;

namespace vitreo::graphics {

namespace {

/// A D3D12 heap (BackendHeap). A GPU-private heap is an MTLHeap of type
/// placement, tracked, so every texture placed in it and its whole-heap buffer
/// alias the same memory and Metal orders work across them. A CPU-visible heap
/// is page-aligned host memory Metal wraps without a copy, which the frontend
/// maps into the guest once: the guest's writes are the GPU's reads.
class MetalBackendHeap final : public BackendHeap {
public:
    MetalBackendHeap(void* device, void* command_queue,
                     const BackendHeapDesc& desc)
        : m_size(desc.size) {
        id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
        if (mtl_device == nil || desc.size == 0) return;
        BackendBufferDesc buffer_desc{};
        buffer_desc.byte_width = static_cast<usize>(desc.size);
        buffer_desc.shader_resource = true;
        buffer_desc.unordered_access = true;
        buffer_desc.vertex_buffer = true;
        buffer_desc.index_buffer = true;
        buffer_desc.constant_buffer = true;
        buffer_desc.cpu_writable = desc.cpu_visible;
        if (desc.cpu_visible) {
            // 64 KiB: a multiple of every host page size, and D3D12's own
            // placement alignment, so every placed window stays page-aligned.
            constexpr u64 kGranule = 64u * 1024u;
            const u64 bytes = (desc.size + kGranule - 1u) / kGranule * kGranule;
            vm_address_t address = 0;
            if (vm_allocate(mach_task_self(), &address,
                            static_cast<vm_size_t>(bytes),
                            VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
                return;
            }
            // The deallocator runs when the last MTLBuffer reference drops --
            // after any command buffer still reading it -- so the pages
            // outlive in-flight GPU work, as the GPU needs.
            id<MTLBuffer> buffer = [mtl_device
                newBufferWithBytesNoCopy:reinterpret_cast<void*>(address)
                                  length:static_cast<NSUInteger>(bytes)
                                 options:MTLResourceStorageModeShared
                             deallocator:^(void* pointer, NSUInteger length) {
                                 (void)vm_deallocate(
                                     mach_task_self(),
                                     reinterpret_cast<vm_address_t>(pointer),
                                     static_cast<vm_size_t>(length));
                             }];
            if (buffer == nil) {
                (void)vm_deallocate(mach_task_self(), address,
                                    static_cast<vm_size_t>(bytes));
                return;
            }
            m_host = reinterpret_cast<void*>(address);
            m_buffer = std::make_unique<MetalBackendBuffer>(
                device, (__bridge void*)buffer, buffer_desc);
            if (!m_buffer->valid()) {
                m_buffer.reset();
                m_host = nullptr;
            }
            return;
        }
        const MTLResourceOptions options =
            MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeTracked;
        id<MTLHeap> heap = nil;
        id<MTLBuffer> buffer = nil;
        if (desc.buffers_only) {
            // Nothing will be placed but windows of the buffer itself.
            buffer = [mtl_device newBufferWithLength:desc.size options:options];
        } else {
            const MTLSizeAndAlign whole =
                [mtl_device heapBufferSizeAndAlignWithLength:desc.size
                                                     options:options];
            MTLHeapDescriptor* heap_desc = [[MTLHeapDescriptor alloc] init];
            heap_desc.type = MTLHeapTypePlacement;
            heap_desc.storageMode = MTLStorageModePrivate;
            heap_desc.hazardTrackingMode = MTLHazardTrackingModeTracked;
            heap_desc.size = std::max<NSUInteger>(whole.size, desc.size);
            heap = [mtl_device newHeapWithDescriptor:heap_desc];
            if (heap == nil) return;
            buffer = [heap newBufferWithLength:desc.size
                                       options:options
                                        offset:0];
        }
        if (buffer == nil) return;
        // Windows zero-fills every new heap unless the application opts out
        // (D3D12_HEAP_FLAG_CREATE_NOT_ZEROED); Metal does not. Committed on
        // the queue now, like texture zero-initialisation, so it runs before
        // anything recorded against the heap afterwards.
        id<MTLCommandQueue> queue =
            (__bridge id<MTLCommandQueue>)command_queue;
        if (queue != nil) {
            @autoreleasepool {
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit =
                    [commands blitCommandEncoder];
                [blit fillBuffer:buffer
                           range:NSMakeRange(0, static_cast<NSUInteger>(
                                                    desc.size))
                           value:0];
                [blit endEncoding];
                [commands commit];
            }
        }
        if (heap != nil) m_heap = (__bridge_retained void*)heap;
        m_buffer = std::make_unique<MetalBackendBuffer>(
            device, (__bridge void*)buffer, buffer_desc);
        if (!m_buffer->valid()) m_buffer.reset();
    }

    ~MetalBackendHeap() override {
        m_buffer.reset();
        if (m_heap != nullptr) CFRelease(m_heap);
    }

    bool valid() const noexcept override { return m_buffer != nullptr; }
    u64 size() const noexcept override { return m_size; }
    void* host_pointer() const noexcept override { return m_host; }
    BackendBuffer* buffer() noexcept override { return m_buffer.get(); }
    /// The id<MTLHeap> textures are placed in; null for a CPU-visible heap.
    [[nodiscard]] void* native_heap() const noexcept { return m_heap; }

private:
    u64 m_size{0};
    void* m_heap{nullptr};
    void* m_host{nullptr};
    std::unique_ptr<MetalBackendBuffer> m_buffer;
};

} // namespace

/// The id<MTLHeap> of a BackendHeap this device created (every one is a
/// MetalBackendHeap: no RTTI, the relationship is by construction).
[[nodiscard]] void* metal_heap_handle(const BackendHeap* heap) {
    return heap != nullptr
               ? static_cast<const MetalBackendHeap*>(heap)->native_heap()
               : nullptr;
}

std::unique_ptr<BackendBuffer> MetalDevice::create_buffer(
    const BackendBufferDesc& desc, const void* initial_data) {
    // A tiled buffer allocates nothing (its tile pools do).
    if (!desc.tiled &&
        !vitreo::memory::host_can_allocate_gpu(desc.byte_width,
                                               gpu_working_set(), "GPU buffer")) {
        return nullptr;
    }
    auto buffer = std::make_unique<MetalBackendBuffer>(
        m_device, desc, initial_data);
    if (!buffer->valid()) return nullptr;
    return buffer;
}

bool MetalDevice::supports_placement_heaps() const noexcept {
    return available();
}

std::unique_ptr<BackendHeap> MetalDevice::create_heap(
    const BackendHeapDesc& desc) {
    if (!available() || desc.size == 0 ||
        !vitreo::memory::host_can_allocate_gpu(desc.size, gpu_working_set(),
                                               "GPU heap")) {
        return nullptr;
    }
    auto heap = std::make_unique<MetalBackendHeap>(m_device, m_command_queue,
                                                   desc);
    if (!heap->valid()) return nullptr;
    return heap;
}

std::unique_ptr<BackendTextureBuffer> MetalDevice::create_texture_buffer(
    const BackendBuffer& buffer, PixelFormat pixel_format,
    unsigned first_element, unsigned num_elements, bool shader_write) {
    // Every BackendBuffer this device hands out is a MetalBackendBuffer; the
    // codebase is built without RTTI, so the relationship is by construction
    // rather than by dynamic_cast, as it is on every other binding path here.
    const auto* metal_buffer = static_cast<const MetalBackendBuffer*>(&buffer);
    if (!metal_buffer->valid()) return nullptr;
    auto view = std::make_unique<MetalBackendTextureBuffer>(
        m_device, *metal_buffer, pixel_format, first_element, num_elements,
        shader_write);
    if (!view->valid()) return nullptr;
    return view;
}

std::unique_ptr<BackendTextureBuffer> MetalDevice::create_mapped_texture_buffer(
    const BackendBuffer& buffer, PixelFormat pixel_format,
    unsigned first_element, unsigned num_elements, u16 component_mapping) {
    const auto* metal_buffer = static_cast<const MetalBackendBuffer*>(&buffer);
    if (!metal_buffer->valid()) return nullptr;
    auto view = std::make_unique<MetalBackendTextureBuffer>(
        m_device, *metal_buffer, pixel_format, first_element, num_elements,
        false, component_mapping);
    if (!view->valid()) return nullptr;
    return view;
}

bool MetalDevice::copy_buffer(BackendBuffer* dst, const BackendBuffer* src) {
    if (!available() || dst == nullptr || src == nullptr) return false;
    if (!dst->valid() || !src->valid()) return false;
    if (dst->desc().byte_width != src->desc().byte_width ||
        dst->desc().byte_width == 0) {
        return false;
    }
    auto* metal_dst = static_cast<MetalBackendBuffer*>(dst);
    const auto* metal_src = static_cast<const MetalBackendBuffer*>(src);
    metal_dst->note_gpu_write();
    void* dst_handle = metal_dst->native_handle();
    void* src_handle = metal_src->native_handle();
    if (dst_handle == nullptr || src_handle == nullptr) return false;
    metal_src->note_gpu_bind();
    // Either buffer may have been renamed into a slice of a shared pool slab,
    // so its storage starts at native_offset(), not at 0. Copying from and to
    // offset 0 of the slab read another buffer's bytes and wrote into a slice
    // that may belong to someone else, while Map(READ) looked at the right one
    // (d3d11_a10_dispatch_readback_probe: 3/20 reads correct, Windows 20/20).
    MetalSubmitQueue::instance().record_copy_buffer(
        src_handle, dst_handle, dst->desc().byte_width,
        metal_src->native_offset(), metal_dst->native_offset());
    return true;
}

bool MetalDevice::copy_buffer_region(BackendBuffer* dst, u64 dst_offset,
                                     const BackendBuffer* src, u64 src_offset,
                                     u64 bytes) {
    if (!available() || dst == nullptr || src == nullptr) return false;
    if (!dst->valid() || !src->valid() || bytes == 0) return false;
    const u64 dst_width = dst->desc().byte_width;
    const u64 src_width = src->desc().byte_width;
    // Refuse rather than clamp: a caller whose range runs past the end has the
    // wrong offset or the wrong size, and a short copy would leave the tail of
    // the destination holding whatever was there before.
    if (dst_offset > dst_width || bytes > dst_width - dst_offset) return false;
    if (src_offset > src_width || bytes > src_width - src_offset) return false;
    // An overlapping blit inside one buffer has an undefined result, so it is
    // refused instead of producing one of the possible answers.
    if (dst == src && dst_offset < src_offset + bytes &&
        src_offset < dst_offset + bytes) {
        return false;
    }
    auto* metal_dst = static_cast<MetalBackendBuffer*>(dst);
    const auto* metal_src = static_cast<const MetalBackendBuffer*>(src);
    // A copy destination is a GPU write: its storage leaves any pool slice
    // first, or the copy would be lost when that slice is rebuilt from the
    // CPU mirror.
    metal_dst->note_gpu_write();
    void* dst_handle = metal_dst->native_handle();
    void* src_handle = metal_src->native_handle();
    if (dst_handle == nullptr || src_handle == nullptr) return false;
    // Without this the buffer is updated in place instead of renamed, and a
    // copy recorded now would land in a buffer the GPU is still reading.
    metal_src->note_gpu_bind();
    // A buffer renamed by a DISCARD lives at native_offset() inside a shared
    // slab, like every draw and dispatch binding accounts for.
    MetalSubmitQueue::instance().record_copy_buffer(
        src_handle, dst_handle, static_cast<usize>(bytes),
        static_cast<usize>(src_offset) + metal_src->native_offset(),
        static_cast<usize>(dst_offset) + metal_dst->native_offset());
    return true;
}

bool MetalDevice::fill_buffer_region(BackendBuffer* dst, u64 offset,
                                     u64 bytes, u8 value) {
    if (!available() || dst == nullptr || !dst->valid() || bytes == 0) {
        return false;
    }
    const u64 width = dst->desc().byte_width;
    if (offset > width || bytes > width - offset) return false;
    if ((offset % 4) != 0 || (bytes % 4) != 0) return false;
    auto* metal_dst = static_cast<MetalBackendBuffer*>(dst);
    // A GPU write, exactly as a copy destination (copy_buffer_region): the
    // storage leaves any pool slice first, so the handle and slab offset are
    // read after it.
    metal_dst->note_gpu_write();
    void* dst_handle = metal_dst->native_handle();
    if (dst_handle == nullptr) return false;
    // The slab offset of a DISCARD-renamed buffer counts toward the
    // granularity too: it is where the blit actually lands.
    const u64 at = offset + metal_dst->native_offset();
    if ((at % 4) != 0) return false;
    MetalSubmitQueue::instance().record_fill_buffer(
        dst_handle, static_cast<usize>(at), static_cast<usize>(bytes), value);
    return true;
}

} // namespace vitreo::graphics
