// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_device_sharing.mm
/// @brief MetalDevice cross-process and IOSurface sharing

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
#include "graphics/metal/metal_backend_texture_2d.hpp"
#include "graphics/metal/metal_backend_texture_3d.hpp"

using namespace vitreo;

namespace vitreo::graphics {

std::unique_ptr<BackendTexture2D> MetalDevice::open_shared_texture_2d(
    u64 handle) {
    if (handle == 0 || handle > UINT32_MAX) return nullptr;
    auto texture = std::make_unique<MetalBackendTexture2D>(
        m_device, m_command_queue, static_cast<u32>(handle));
    if (!texture->valid()) return nullptr;
    return texture;
}

bool MetalDevice::supports_shared_texture_2d(PixelFormat format) const noexcept {
    const FormatInfo info = format_table(format);
    return m_device != nullptr && info.mtl_pixel_format != 0 &&
           iosurface_bytes_per_element(info.mtl_pixel_format) != 0;
}

std::shared_ptr<void> MetalDevice::retain_shared_texture(u64 handle) {
    if (handle == 0 || handle > UINT32_MAX) return nullptr;
    // IOSurfaceLookup returns a +1 reference: the surface (and so the pixels
    // every opener aliases) lives until the returned reference is dropped.
    IOSurfaceRef surface = IOSurfaceLookup(static_cast<IOSurfaceID>(handle));
    if (surface == nullptr) return nullptr;
    return std::shared_ptr<void>(const_cast<void*>(
                                     static_cast<const void*>(surface)),
                                 [](void* retained) {
                                     CFRelease(static_cast<CFTypeRef>(retained));
                                 });
}

bool MetalDevice::supports_shared_texture(PixelFormat format) const noexcept {
    if (m_device == nullptr || format == PixelFormat::Unknown ||
        static_cast<u32>(format) >= static_cast<u32>(PixelFormat::Count)) {
        return false;
    }
    // Shareable in every shape when the store can hold it: every format with
    // a texel or block size, and the depth / stencil ones by aspect.
    SharedStoreRecord record{};
    record.kind = SharedStoreKind::Texture2D;
    record.pixel_format = static_cast<u32>(format);
    record.width = 4;
    record.height = 4;
    usize bytes = 0;
    return !shared_store_layout(record, bytes).empty();
}

std::shared_ptr<BackendSharedMemory> MetalDevice::create_shared_memory(
    usize bytes, std::vector<u8> metadata) {
    if (m_device == nullptr || bytes == 0 || bytes > UINT32_MAX) {
        return nullptr;
    }
    SharedStoreRecord record{};
    record.kind = SharedStoreKind::Memory;
    record.width = static_cast<u32>(bytes);
    record.frontend = metadata;
    auto local = std::make_shared<MetalSharedLocal>(m_device, record);
    if (!local->create_store()) return nullptr;
    MetalSharedRegistry::instance().add(local,
                                        local->store()->payload_buffer());
    return std::make_shared<MetalSharedMemory>(std::move(local),
                                               std::move(metadata));
}

GraphicsBackendDevice::SharedOpening MetalDevice::open_shared_resource(
    u64 handle) {
    SharedOpening opening;
    if (m_device == nullptr || handle == 0 || handle > UINT32_MAX) {
        return opening;
    }
    const u32 id = static_cast<u32>(handle);
    auto& registry = MetalSharedRegistry::instance();
    // An opening in the process that already holds the resource shares its
    // local storage; one in another process mirrors the store.
    std::shared_ptr<MetalSharedLocal> local = registry.find_store(id);
    std::shared_ptr<MetalSharedStore> store;
    if (!local) {
        store = MetalSharedStore::open(m_device, id);
        if (!store) {
            // Not a store: a zero-copy shared texture (or nothing).
            return GraphicsBackendDevice::open_shared_resource(handle);
        }
    }
    const SharedStoreRecord record = local ? local->record() : store->record();
    opening.metadata = record.frontend;
    switch (record.kind) {
    case SharedStoreKind::Texture2D: {
        BackendTexture2DDesc desc =
            MetalBackendTexture2D::desc_from_record(record);
        if (local) {
            opening.texture_2d = std::make_unique<MetalBackendTexture2D>(
                m_device, m_command_queue, local, std::move(desc));
        } else {
            desc.shareable = false;
            desc.shared_mirror = true;
            auto texture = std::make_unique<MetalBackendTexture2D>(
                m_device, m_command_queue, desc, nullptr, 0);
            if (!texture->valid()) return {};
            local = std::make_shared<MetalSharedLocal>(m_device, record);
            local->texture = texture->local_texture();
            local->plane1 = texture->local_chroma_texture();
            registry.add(local, local->texture->handle());
            if (local->plane1) registry.add(local, local->plane1->handle());
            local->attach_store(std::move(store));
            texture->adopt_shared(local);
            opening.texture_2d = std::move(texture);
            registry.acquire();
        }
        if (!opening.texture_2d->valid()) return {};
        opening.kind = SharedOpening::Kind::Texture2D;
        return opening;
    }
    case SharedStoreKind::Texture3D: {
        BackendTexture3DDesc desc =
            MetalBackendTexture3D::desc_from_record(record);
        if (local) {
            opening.texture_3d = std::make_unique<MetalBackendTexture3D>(
                m_device, m_command_queue, local, std::move(desc));
        } else {
            desc.shareable = false;
            auto texture = std::make_unique<MetalBackendTexture3D>(
                m_device, m_command_queue, desc, nullptr, 0, 0);
            if (!texture->valid()) return {};
            local = std::make_shared<MetalSharedLocal>(m_device, record);
            local->texture = texture->local_texture();
            registry.add(local, local->texture->handle());
            local->attach_store(std::move(store));
            texture->adopt_shared(local);
            opening.texture_3d = std::move(texture);
            registry.acquire();
        }
        if (!opening.texture_3d->valid()) return {};
        opening.kind = SharedOpening::Kind::Texture3D;
        return opening;
    }
    case SharedStoreKind::Buffer: {
        BackendBufferDesc desc = MetalBackendBuffer::desc_from_record(record);
        if (!local) {
            local = std::make_shared<MetalSharedLocal>(m_device, record);
            local->attach_store(std::move(store));
        }
        opening.buffer = std::make_unique<MetalBackendBuffer>(
            m_device, local, std::move(desc));
        if (!opening.buffer->valid()) return {};
        opening.kind = SharedOpening::Kind::Buffer;
        registry.acquire();
        return opening;
    }
    case SharedStoreKind::Memory: {
        if (!local) {
            local = std::make_shared<MetalSharedLocal>(m_device, record);
            local->attach_store(std::move(store));
            registry.add(local, local->store()->payload_buffer());
        }
        opening.memory =
            std::make_shared<MetalSharedMemory>(local, record.frontend);
        opening.kind = SharedOpening::Kind::Memory;
        return opening;
    }
    }
    return {};
}

std::shared_ptr<GpuCompletion> MetalDevice::publish_shared_writes() {
    if (!available()) return nullptr;
    auto versions = MetalSharedRegistry::instance().push_dirty();
    if (versions.empty()) return nullptr;
    auto completion = flush_completion();
    MetalSharedRegistry::publish_after(completion, std::move(versions));
    return completion;
}

void MetalDevice::acquire_shared_writes() {
    MetalSharedRegistry::instance().acquire();
}

namespace {

/// The Metal texture a copy location names (2D or volume).
void* location_texture(const BackendTextureLocation& location) {
    if (location.texture_2d != nullptr) {
        return static_cast<const MetalBackendTexture2D*>(location.texture_2d)
            ->native_handle();
    }
    if (location.texture_3d != nullptr) {
        return static_cast<const MetalBackendTexture3D*>(location.texture_3d)
            ->native_handle();
    }
    return nullptr;
}

}  // namespace

bool MetalDevice::copy_texture_to_shared_memory(
    const BackendTextureLocation& src, unsigned width, unsigned height,
    unsigned depth, BackendSharedMemory& dst, usize offset, usize row_pitch,
    usize image_pitch) {
    void* texture = location_texture(src);
    const auto store = static_cast<MetalSharedMemory&>(dst).store();
    if (texture == nullptr || !store || width == 0 || height == 0 ||
        depth == 0 || row_pitch == 0 ||
        offset + (depth > 1 ? image_pitch * depth : row_pitch * height) >
            store->payload_size()) {
        return false;
    }
    MetalSubmitQueue::instance().record_download_texture(
        texture, src.mip, src.slice, src.x, src.y, src.z, width, height,
        depth, store->payload_buffer(), row_pitch,
        depth > 1 ? image_pitch : 0, offset);
    return true;
}

bool MetalDevice::copy_shared_memory_to_texture(
    BackendSharedMemory& src, usize offset, usize row_pitch,
    usize image_pitch, const BackendTextureLocation& dst, unsigned width,
    unsigned height, unsigned depth) {
    void* texture = location_texture(dst);
    const auto store = static_cast<MetalSharedMemory&>(src).store();
    if (texture == nullptr || !store || width == 0 || height == 0 ||
        depth == 0 || row_pitch == 0 ||
        offset + (depth > 1 ? image_pitch * depth : row_pitch * height) >
            store->payload_size()) {
        return false;
    }
    MetalSubmitQueue::instance().record_upload_texture(
        store->payload_buffer(), texture, row_pitch,
        depth > 1 ? image_pitch : 0, width, height, depth, dst.mip,
        dst.slice, dst.x, dst.y, dst.z, 0, offset);
    return true;
}

} // namespace vitreo::graphics
