// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_backend_texture_2d.mm
/// @brief MetalBackendTexture2D creation and storage lifecycle

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
#include "graphics/metal/metal_backend_texture_ops.hpp"

using namespace vitreo;

namespace vitreo::graphics {

namespace {

/// A texture the guest asked for could not be created. The D3D frontends turn
/// this into E_FAIL / E_OUTOFMEMORY with nothing else in the log, so say which
/// step failed and how much memory the device and process hold.
void report_creation_failure(void* device, const BackendTexture2DDesc& desc,
                             const char* why) {
    u64 device_bytes = 0;
    if (device != nullptr) {
        device_bytes = static_cast<u64>(
            ((__bridge id<MTLDevice>)device).currentAllocatedSize);
    }
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const u64 footprint =
        task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS
            ? vm.phys_footprint
            : 0;
    std::fprintf(stderr,
                 "[metal] texture2d creation failed (%s): %ux%u mips=%u "
                 "array=%u fmt=%u samples=%u rt=%d ds=%d uav=%d | device "
                 "allocated %llu MiB, process footprint %llu MiB\n",
                 why, desc.width, desc.height, desc.mip_levels,
                 desc.array_size, static_cast<unsigned>(desc.pixel_format),
                 desc.sample_count, desc.render_target ? 1 : 0,
                 desc.depth_stencil ? 1 : 0, desc.unordered_access ? 1 : 0,
                 static_cast<unsigned long long>(device_bytes >> 20),
                 static_cast<unsigned long long>(footprint >> 20));
}

} // namespace

/// The MetalTexture description of @p desc, shared by creation and by the
/// placement-heap size query (heap_texture_placement) so a placed
/// texture is exactly the one its size was computed for. Returns null on
/// success, else why the description is refused ("" when silently).
/// @p cube_faces receives the face count of a cube.
[[nodiscard]] const char* MetalBackendTexture2D::describe(
    const BackendTexture2DDesc& desc, TextureDesc& metal_desc,
    u32& cube_faces) {
    const FormatInfo format = format_table(desc.pixel_format);
    const PixelFormat chroma_format =
        planar_chroma_format(desc.pixel_format);
    if (chroma_format != PixelFormat::Unknown &&
        (desc.width % 2u != 0 || desc.height % 2u != 0 ||
         desc.mip_levels != 1 || desc.sample_count > 1 || desc.cube)) {
        // Learn DXGI_FORMAT NV12 / P010 / P016: "Width and height must
        // be even"; one mip, no MSAA, no cube (measured,
        // d3d11_nv12_probe [create]).
        return "planar video shape";
    }
    if (format.mtl_pixel_format == 0 || desc.width == 0 ||
        desc.height == 0 || desc.mip_levels == 0 ||
        desc.array_size == 0) {
        return format.mtl_pixel_format == 0 ? "no Metal format"
                                            : "zero extent";
    }
    cube_faces = desc.array_size;
    if (desc.cube) {
        if (desc.width != desc.height) {
            return "";
        }
        // D3D11 counts faces (6, 12, …). D3D9 CreateCubeTexture uses
        // array_size=1 for one cube; treat that as six faces.
        if (cube_faces == 1) {
            cube_faces = 6;
        }
        if (cube_faces < 6 || (cube_faces % 6) != 0) {
            return "";
        }
    }
    metal_desc.width = desc.width;
    metal_desc.height = desc.height;
    metal_desc.mip_levels = desc.mip_levels;
    // Face count → Metal arrayLength (number of cubes).
    metal_desc.array_size = desc.cube ? cube_faces / 6u
                                     : desc.array_size;
    metal_desc.texture_type = desc.cube
        ? (cube_faces > 6 ? 6u /* MTLTextureTypeCubeArray */
                          : 5u /* MTLTextureTypeCube */)
        : desc.array_size > 1 ? 3u /* MTLTextureType2DArray */
                              : 2u /* MTLTextureType2D */;
    metal_desc.pixel_format = format.mtl_pixel_format;
    metal_desc.usage =
        (desc.shader_resource ? MTLTextureUsageShaderRead : 0) |
        (desc.render_target || desc.depth_stencil
             ? MTLTextureUsageRenderTarget
             : 0) |
        (desc.unordered_access ? MTLTextureUsageShaderWrite : 0) |
        ((desc.shader_resource || desc.unordered_access) &&
         (desc.mip_levels > 1 || desc.array_size > 1)
             ? MTLTextureUsagePixelFormatView
             : 0) |
        // A UAV clear stores raw bits through an unsigned-integer view of
        // the same texel size (fill_texels).
        (desc.unordered_access ? MTLTextureUsagePixelFormatView : 0) |
        (desc.pixel_format_view ? MTLTextureUsagePixelFormatView : 0) |
        // A planar plane is viewed as UNORM or UINT (Learn DXGI_FORMAT:
        // R8_UNORM / R8_UINT luma views, R8G8 chroma views; R32_UINT
        // chroma UAVs of P010).
        (chroma_format != PixelFormat::Unknown
             ? MTLTextureUsagePixelFormatView
             : 0);
    // Resource ownership is determined by the D3D usage contract, not by
    // whether CreateTexture supplied initial bytes. DEFAULT textures stay
    // GPU-private and receive those bytes through the same staging blit as
    // UpdateSubresource; otherwise mip 0 alone becomes a shared texture
    // while the remaining mips follow the private-resource path. Dynamic
    // textures remain shared because Map exposes CPU writes directly.
    metal_desc.storage_mode = desc.cpu_writable
        ? MTLStorageModeShared
        : MTLStorageModePrivate;
    metal_desc.render_target = desc.render_target || desc.depth_stencil;
    // A planar texture's planes are two textures: shared through the
    // store, never as a single-plane IOSurface of its luma alone.
    metal_desc.shareable =
        desc.shareable && chroma_format == PixelFormat::Unknown;
    if (desc.shareable) {
        // Everything an opener needs to rebuild this texture's backend
        // description; MetalTexture adds the Metal fields it allocated.
        SharedTextureRecord record{};
        record.backend_format = static_cast<u32>(desc.pixel_format);
        record.backend_flags =
            (desc.render_target ? SharedTextureRecord::kRenderTarget : 0u) |
            (desc.shader_resource ? SharedTextureRecord::kShaderResource
                                  : 0u) |
            (desc.unordered_access ? SharedTextureRecord::kUnorderedAccess
                                   : 0u) |
            (desc.pixel_format_view ? SharedTextureRecord::kPixelFormatView
                                    : 0u);
        record.frontend = desc.shared_metadata;
        metal_desc.shared_record = record.encode();
    }
    if (desc.sample_count > 1) {
        metal_desc.sample_count = desc.sample_count;
        metal_desc.texture_type = desc.array_size > 1
            ? 8u  // MTLTextureType2DMultisampleArray
            : 4u; // MTLTextureType2DMultisample
        metal_desc.mip_levels = 1;
        metal_desc.array_size = desc.array_size;
        metal_desc.storage_mode = MTLStorageModePrivate;
        metal_desc.usage =
            (desc.shader_resource ? MTLTextureUsageShaderRead : 0) |
            (desc.render_target ? MTLTextureUsageRenderTarget : 0) |
            (desc.pixel_format_view ? MTLTextureUsagePixelFormatView : 0);
        if (desc.tiled) {
            // CopyTiles / UpdateTiles read every sample in a kernel and
            // write it per sample, colour through an unsigned view
            // (MetalTiled::copy_multisampled).
            metal_desc.usage |= MTLTextureUsageShaderRead |
                                (format.is_depth
                                     ? 0
                                     : MTLTextureUsagePixelFormatView);
        }
        if (desc.shareable || desc.shared_mirror) {
            // Shared contents move per sample through a compute read of
            // an unsigned-integer (or stencil-plane) view and a
            // per-sample render (metal_shared_resource.mm).
            metal_desc.usage |= MTLTextureUsageShaderRead |
                                MTLTextureUsagePixelFormatView |
                                MTLTextureUsageRenderTarget;
        }
        metal_desc.shareable = false;
    }
    switch (desc.channel_swizzle) {
    case BackendTexture2DDesc::ChannelSwizzle::Luminance:
        metal_desc.channel_swizzle = 1u;
        break;
    case BackendTexture2DDesc::ChannelSwizzle::LuminanceAlpha:
        metal_desc.channel_swizzle = 2u;
        break;
    default:
        metal_desc.channel_swizzle = 0u;
        break;
    }
    // A tiled texture (D3D11_RESOURCE_MISC_TILED) is a placement sparse
    // texture: memory comes from tile pools (metal_tiled.hpp).
    metal_desc.placement_sparse = desc.tiled;
    if (desc.tiled) {
        metal_desc.shareable = false;
        metal_desc.storage_mode = MTLStorageModePrivate;
    }
    if (desc.heap != nullptr) {
        // Placed in a D3D12 heap: the heap's storage, never shared or
        // tiled (CreatePlacedResource), and never renamed to shared
        // storage on failure.
        metal_desc.heap_placed = true;
        metal_desc.placement_heap = metal_heap_handle(desc.heap);
        metal_desc.heap_offset = desc.heap_offset;
        metal_desc.shareable = false;
        metal_desc.shared_record.clear();
    }
    return nullptr;
}

MetalBackendTexture2D::MetalBackendTexture2D(
    void* device, void* command_queue, BackendTexture2DDesc desc,
    const void* initial_data, unsigned initial_row_pitch)
    : m_desc(desc)
    , m_command_queue(command_queue) {
    TextureDesc metal_desc{};
    u32 cube_faces = desc.array_size;
    if (const char* failure = describe(desc, metal_desc, cube_faces)) {
        if (failure[0] != '\0') {
            report_creation_failure(device, desc, failure);
        }
        return;
    }
    if (desc.cube) m_desc.array_size = cube_faces;
    const FormatInfo format = format_table(desc.pixel_format);
    const PixelFormat chroma_format =
        planar_chroma_format(desc.pixel_format);
    m_texture = std::make_shared<MetalTexture>(device, metal_desc);
    m_device = device;
    m_metal_desc = metal_desc;
    if (m_texture->handle() == nullptr && metal_desc.render_target &&
        !desc.tiled && desc.heap == nullptr &&
        metal_desc.storage_mode != 0u /* MTLStorageModeShared */) {
        // AMD private cube/RT allocations can return nil. Shared storage
        // is the same fallback the swapchain already uses.
        metal_desc.storage_mode = 0u;
        m_texture = std::make_shared<MetalTexture>(device, metal_desc);
        m_metal_desc = metal_desc;
    }
    if (m_texture->handle() == nullptr) {
        report_creation_failure(device, desc, "newTextureWithDescriptor nil");
        m_texture.reset();
        return;
    }
    if (chroma_format != PixelFormat::Unknown) {
        // The chroma plane: a texture of its own at half size, which
        // every chroma operation delegates to (planar_chroma()).
        BackendTexture2DDesc chroma = desc;
        chroma.pixel_format = chroma_format;
        chroma.width = desc.width / 2u;
        chroma.height = desc.height / 2u;
        chroma.shareable = false;
        chroma.shared_metadata.clear();
        chroma.pixel_format_view = true;
        m_plane1 = std::make_unique<MetalBackendTexture2D>(
            device, command_queue, chroma, nullptr, 0);
        if (!m_plane1->valid()) {
            report_creation_failure(device, desc, "chroma plane");
            m_texture.reset();
            m_plane1.reset();
            return;
        }
    }
    // A shareable texture that did not land on an IOSurface (several
    // mips, slices or samples, or a format without an IOSurface form) is
    // mirrored through a canonical store, allocated at its first export
    // (metal_shared_resource.hpp).
    if (desc.shareable && m_texture->shared_handle() == 0) {
        SharedStoreRecord record = shared_record_for(desc);
        record.frontend = desc.shared_metadata;
        m_shared = std::make_shared<MetalSharedLocal>(device,
                                                      std::move(record));
        m_shared->texture = m_texture;
        if (m_plane1) m_shared->plane1 = m_plane1->m_texture;
        MetalSharedRegistry::instance().add(m_shared, m_texture->handle());
        if (m_plane1) {
            MetalSharedRegistry::instance().add(
                m_shared, m_plane1->native_handle());
        }
    }
    m_desc.shareable = m_texture->shared_handle() != 0 || m_shared;
    if (!m_desc.shareable) m_desc.shared_metadata.clear();
    if (initial_data != nullptr && desc.sample_count <= 1) {
        usize row_pitch = initial_row_pitch;
        if (row_pitch == 0) {
            if (format.is_compressed) {
                const u32 block = compressed_block_bytes(desc.pixel_format);
                const u32 blocks_w = (desc.width + 3u) / 4u;
                row_pitch = static_cast<usize>(blocks_w) * block;
            } else if (format.bytes_per_pixel != 0) {
                row_pitch = static_cast<usize>(desc.width) *
                            format.bytes_per_pixel;
            }
        }
        if (row_pitch != 0) {
            (void)update_slice(0, 0, initial_data,
                               static_cast<unsigned>(row_pitch));
        }
    } else if (initial_data == nullptr && !desc.tiled &&
               desc.heap == nullptr &&
               metal_desc.storage_mode != 0u /* not Shared */) {
        // A tiled texture has no memory of its own to zero: every tile
        // starts unmapped, and tile-pool contents start undefined
        // (Learn, "Tiled resource creation parameters"). A texture placed
        // in a heap is a window of memory the heap zeroed when it was
        // created and other placed resources may since have written:
        // placing it must not touch those bytes (Learn, "Memory aliasing
        // and data inheritance").
        zero_initialize(format);
    }
}

/// Windows (WDDM) zero-fills every new GPU allocation so no process can
/// read another's leftovers; a D3D resource created without initial data
/// therefore reads as zero until written, and games rely on it. A D3D11
/// renderer allocates pooled targets it samples before (or without) writing
/// them. Metal gives no such guarantee: a fresh private texture reads as
/// garbage (Apple GPUs show magenta / NaN), which painted one frame's
/// whole scene magenta. Shared storage is already zeroed by the OS.
//
// Everything here is committed on the Metal queue IMMEDIATELY, not
// recorded into the submit stream: CPU uploads to private textures
// (update_slice -> blit_cpu_to_texture) commit immediately too, and a
// stream-recorded zero clear would land AFTER an UpdateSubresource that
// followed creation, wiping the guest's data.
void MetalBackendTexture2D::zero_initialize(const FormatInfo& format) {
    const unsigned layers = std::max(1u, m_desc.array_size);
    void* queue_ptr0 = m_command_queue != nullptr
                           ? m_command_queue
                           : MetalDevice::instance().command_queue_ptr();
    void* handle0 = native_handle();
    if (queue_ptr0 == nullptr || handle0 == nullptr) return;
    if (m_desc.depth_stencil || m_desc.render_target) {
        @autoreleasepool {
            id<MTLTexture> texture = (__bridge id<MTLTexture>)handle0;
            id<MTLCommandQueue> queue =
                (__bridge id<MTLCommandQueue>)queue_ptr0;
            id<MTLCommandBuffer> command = [queue commandBuffer];
            const bool stencil = format.mtl_pixel_format == 255 ||
                                 format.mtl_pixel_format == 260;
            for (unsigned mip = 0; mip < m_desc.mip_levels; ++mip) {
                MTLRenderPassDescriptor* pass =
                    [MTLRenderPassDescriptor renderPassDescriptor];
                if (m_desc.depth_stencil) {
                    pass.depthAttachment.texture = texture;
                    pass.depthAttachment.level = mip;
                    pass.depthAttachment.loadAction = MTLLoadActionClear;
                    pass.depthAttachment.storeAction = MTLStoreActionStore;
                    pass.depthAttachment.clearDepth = 0.0;
                    if (stencil) {
                        pass.stencilAttachment.texture = texture;
                        pass.stencilAttachment.level = mip;
                        pass.stencilAttachment.loadAction =
                            MTLLoadActionClear;
                        pass.stencilAttachment.storeAction =
                            MTLStoreActionStore;
                        pass.stencilAttachment.clearStencil = 0;
                    }
                } else {
                    pass.colorAttachments[0].texture = texture;
                    pass.colorAttachments[0].level = mip;
                    pass.colorAttachments[0].loadAction =
                        MTLLoadActionClear;
                    pass.colorAttachments[0].storeAction =
                        MTLStoreActionStore;
                    pass.colorAttachments[0].clearColor =
                        MTLClearColorMake(0, 0, 0, 0);
                }
                // A layered pass clears every slice of an array/cube.
                if (texture.textureType == MTLTextureType2DArray ||
                    texture.textureType == MTLTextureTypeCube ||
                    texture.textureType == MTLTextureTypeCubeArray ||
                    texture.textureType ==
                        MTLTextureType2DMultisampleArray) {
                    pass.renderTargetArrayLength =
                        texture.textureType == MTLTextureTypeCube
                            ? 6u : texture.arrayLength *
                               (texture.textureType ==
                                        MTLTextureTypeCubeArray ? 6u : 1u);
                }
                id<MTLRenderCommandEncoder> enc =
                    [command renderCommandEncoderWithDescriptor:pass];
                [enc endEncoding];
            }
            [command commit];
        }
        return;
    }
    (void)layers;
    if (m_desc.sample_count > 1) return;  // not blit-writable
    // Any other private texture: blit zeros into every mip and slice.
    const u32 block = format.is_compressed
        ? compressed_block_bytes(m_desc.pixel_format) : 0u;
    const u32 pixel = format.bytes_per_pixel;
    if (format.is_compressed ? block == 0 : pixel == 0) return;
    const u32 w0 = std::max(1u, m_desc.width);
    const u32 h0 = std::max(1u, m_desc.height);
    const usize row0 = format.is_compressed
        ? static_cast<usize>((w0 + 3u) / 4u) * block
        : static_cast<usize>(w0) * pixel;
    const usize rows0 = format.is_compressed ? (h0 + 3u) / 4u : h0;
    void* queue_ptr = m_command_queue != nullptr
                          ? m_command_queue
                          : MetalDevice::instance().command_queue_ptr();
    void* handle = native_handle();
    if (queue_ptr == nullptr || handle == nullptr) return;
    @autoreleasepool {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)handle;
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)queue_ptr;
        // One zeroed source big enough for mip 0 of one slice; smaller
        // mips reuse its prefix. Shared buffers are zero-filled.
        id<MTLBuffer> zeros =
            [texture.device newBufferWithLength:row0 * rows0
                                        options:MTLResourceStorageModeShared];
        if (zeros == nil) return;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        const unsigned slices = texture.textureType == MTLTextureTypeCube
            ? 6u * std::max(1u, layers / 6u) : layers;
        for (unsigned mip = 0; mip < m_desc.mip_levels; ++mip) {
            const u32 w = std::max(1u, w0 >> mip);
            const u32 h = std::max(1u, h0 >> mip);
            const usize row = format.is_compressed
                ? static_cast<usize>((w + 3u) / 4u) * block
                : static_cast<usize>(w) * pixel;
            const usize rows = format.is_compressed ? (h + 3u) / 4u : h;
            for (unsigned slice = 0; slice < slices; ++slice) {
                [blit copyFromBuffer:zeros
                        sourceOffset:0
                   sourceBytesPerRow:row
                 sourceBytesPerImage:row * rows
                          sourceSize:MTLSizeMake(w, h, 1)
                           toTexture:texture
                    destinationSlice:slice
                    destinationLevel:mip
                   destinationOrigin:MTLOriginMake(0, 0, 0)];
            }
        }
        [blit endEncoding];
        [command commit];
    }
}

/// Another opening, in this process, of the mirrored shared texture
/// @p local: the same local Metal texture (every device of the process
/// records into one queue).
MetalBackendTexture2D::MetalBackendTexture2D(
    void* device, void* command_queue, std::shared_ptr<MetalSharedLocal> local,
    BackendTexture2DDesc desc)
    : m_desc(std::move(desc))
    , m_command_queue(command_queue)
    , m_shared(std::move(local)) {
    m_texture = m_shared->texture;
    m_device = device;
    if (m_texture) m_metal_desc = m_texture->desc();
    m_desc.shareable = true;
    const PixelFormat chroma_format =
        planar_chroma_format(m_desc.pixel_format);
    if (chroma_format != PixelFormat::Unknown && m_shared->plane1) {
        BackendTexture2DDesc chroma = m_desc;
        chroma.pixel_format = chroma_format;
        chroma.width = m_desc.width / 2u;
        chroma.height = m_desc.height / 2u;
        chroma.shareable = false;
        chroma.shared_metadata.clear();
        chroma.pixel_format_view = true;
        m_plane1 = std::unique_ptr<MetalBackendTexture2D>(
            new MetalBackendTexture2D(device, command_queue,
                                      m_shared->plane1, chroma));
    }
}

/// The description of a mirrored texture this backend records in its
/// store, from which every opener rebuilds it.
[[nodiscard]] SharedStoreRecord MetalBackendTexture2D::shared_record_for(
    const BackendTexture2DDesc& desc) {
    SharedStoreRecord record{};
    record.kind = SharedStoreKind::Texture2D;
    record.pixel_format = static_cast<u32>(desc.pixel_format);
    record.width = desc.width;
    record.height = desc.height;
    record.depth = 1;
    record.mip_levels = desc.sample_count > 1 ? 1u : desc.mip_levels;
    record.array_size =
        desc.cube && desc.array_size == 1 ? 6u : desc.array_size;
    record.sample_count = std::max(1u, desc.sample_count);
    record.flags =
        (desc.cube ? SharedStoreRecord::kCube : 0u) |
        (desc.render_target ? SharedStoreRecord::kRenderTarget : 0u) |
        (desc.shader_resource ? SharedStoreRecord::kShaderResource : 0u) |
        (desc.unordered_access ? SharedStoreRecord::kUnorderedAccess
                               : 0u) |
        (desc.depth_stencil ? SharedStoreRecord::kDepthStencil : 0u) |
        (desc.pixel_format_view ? SharedStoreRecord::kPixelFormatView
                                : 0u);
    return record;
}

/// The backend description @p record names (the inverse of
/// shared_record_for).
[[nodiscard]] BackendTexture2DDesc MetalBackendTexture2D::desc_from_record(
    const SharedStoreRecord& record) {
    BackendTexture2DDesc desc{};
    desc.width = record.width;
    desc.height = record.height;
    desc.mip_levels = record.mip_levels;
    desc.array_size = record.array_size;
    desc.pixel_format = static_cast<PixelFormat>(record.pixel_format);
    desc.cube = (record.flags & SharedStoreRecord::kCube) != 0;
    desc.render_target =
        (record.flags & SharedStoreRecord::kRenderTarget) != 0;
    desc.shader_resource =
        (record.flags & SharedStoreRecord::kShaderResource) != 0;
    desc.unordered_access =
        (record.flags & SharedStoreRecord::kUnorderedAccess) != 0;
    desc.depth_stencil =
        (record.flags & SharedStoreRecord::kDepthStencil) != 0;
    desc.pixel_format_view =
        (record.flags & SharedStoreRecord::kPixelFormatView) != 0;
    desc.sample_count = record.sample_count;
    desc.shared_metadata = record.frontend;
    return desc;
}

MetalBackendTexture2D::MetalBackendTexture2D(void* device, void* command_queue,
                                             u32 shared_handle)
    : m_command_queue(command_queue) {
    m_texture = std::make_shared<MetalTexture>(device, shared_handle);
    if (m_texture->handle() == nullptr) {
        m_texture.reset();
        return;
    }
    const TextureDesc& opened = m_texture->desc();
    m_desc.width = opened.width;
    m_desc.height = opened.height;
    m_desc.mip_levels = 1;
    m_desc.array_size = 1;
    m_desc.shareable = true;
    SharedTextureRecord record{};
    if (SharedTextureRecord::decode(opened.shared_record.data(),
                                    opened.shared_record.size(), record) &&
        record.backend_format != 0u &&
        record.backend_format < static_cast<u32>(PixelFormat::Count) &&
        format_table(static_cast<PixelFormat>(record.backend_format))
                .mtl_pixel_format == record.mtl_pixel_format) {
        // The creator's own description, as it stored it.
        m_desc.pixel_format = static_cast<PixelFormat>(record.backend_format);
        const u32 flags = record.backend_flags;
        m_desc.render_target =
            (flags & SharedTextureRecord::kRenderTarget) != 0;
        m_desc.shader_resource =
            (flags & SharedTextureRecord::kShaderResource) != 0;
        m_desc.unordered_access =
            (flags & SharedTextureRecord::kUnorderedAccess) != 0;
        m_desc.pixel_format_view =
            (flags & SharedTextureRecord::kPixelFormatView) != 0;
        m_desc.shared_metadata = std::move(record.frontend);
    } else {
        // A surface this backend did not describe (MetalTexture accepted
        // it only as 32-bit BGRA).
        m_desc.pixel_format = PixelFormat::BGRA8Unorm;
        m_desc.render_target = true;
        m_desc.shader_resource = true;
    }
    m_device = device;
    m_metal_desc = opened;
}

bool MetalBackendTexture2D::discard_storage() {
    if (!m_desc.cpu_writable || m_texture == nullptr ||
        m_borrowed_handle != nullptr || m_device == nullptr) {
        return false;
    }
    auto& queue = MetalSubmitQueue::instance();
    // Work recorded so far belongs to the recording generation; once the
    // GPU has retired it the old storage is free for a later rename.
    const u64 retire = queue.recording_generation();
    const u64 completed = queue.completed_generation();
    std::shared_ptr<MetalTexture> next;
    for (auto it = m_discarded.begin(); it != m_discarded.end(); ++it) {
        if (it->second <= completed) {
            next = std::move(it->first);
            m_discarded.erase(it);
            break;
        }
    }
    if (next == nullptr) {
        next = std::make_shared<MetalTexture>(m_device, m_metal_desc);
        if (next->handle() == nullptr) return false;
    }
    // Recorded ops retain the Metal texture they bind, so dropping the
    // oldest parked storage never frees something still queued.
    m_discarded.emplace_back(std::move(m_texture), retire);
    if (m_discarded.size() > 3) m_discarded.erase(m_discarded.begin());
    m_texture = std::move(next);
    for (const auto& [key, view] : m_shader_views) {
        (void)key;
        CFRelease(view);
    }
    m_shader_views.clear();
    m_view_memo = {};
    m_storage_fresh = true;
    return true;
}

MetalBackendTexture2D::~MetalBackendTexture2D() {
    for (const auto& [key, view] : m_shader_views) {
        (void)key;
        CFRelease(view);
    }
    m_shader_views.clear();
    m_view_memo = {};
}

std::unique_ptr<BackendTexture2D> MetalDevice::create_texture_2d(
    const BackendTexture2DDesc& desc, const void* initial_data,
    unsigned initial_row_pitch) {
    const unsigned layers =
        desc.cube && desc.array_size == 1 ? 6u : desc.array_size;
    if (!desc.tiled && !vitreo::memory::host_can_allocate_gpu(
            estimated_texture_bytes(desc.pixel_format, desc.width,
                                    desc.height, 1, desc.mip_levels, layers,
                                    desc.sample_count, initial_data != nullptr),
            gpu_working_set(), "GPU texture2d")) {
        // The admission guard runs before Metal allocation, so it otherwise
        // bypasses the descriptor/physical-usage failure diagnostic below.
        // Report the first refusal without turning repeated allocation probes
        // into unbounded logging; preserve the guard's refusal marker.
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set(std::memory_order_relaxed)) {
            report_creation_failure(m_device, desc, "host memory floor");
        }
        return nullptr;
    }
    auto texture = std::make_unique<MetalBackendTexture2D>(
        m_device, m_command_queue, desc, initial_data, initial_row_pitch);
    if (!texture->valid()) return nullptr;
    return texture;
}

bool MetalDevice::heap_texture_placement(const BackendTexture2DDesc& desc,
                                         u64& size, u64& alignment) {
    if (!available() || desc.tiled || desc.shareable) return false;
    TextureDesc metal_desc{};
    u32 cube_faces = desc.array_size;
    if (MetalBackendTexture2D::describe(desc, metal_desc, cube_faces) !=
        nullptr) {
        return false;
    }
    metal_desc.heap_placed = true;
    metal_desc.placement_heap = nullptr;
    return MetalTexture::placement_size_and_align(m_device, metal_desc, size,
                                                  alignment);
}

} // namespace vitreo::graphics
