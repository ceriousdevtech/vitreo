/// @file metal_texture.mm
/// @brief Metal GPU texture implementation

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include "graphics/metal/metal_texture.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/contracts/format_converter.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vitreo::graphics {
namespace {

/// Metal aborts the process from -[MTLTextureDescriptor validateWithDevice:]
/// when a descriptor names a pixel format the device does not implement, so an
/// unmappable guest format must be rejected before it reaches Metal. Vitreo's
/// contract is to fail the guest's creation call honestly, not to die.
///
/// 0 is the converters' "no Metal equivalent" result. 255
/// (Depth24Unorm_Stencil8) is unimplemented on every Apple-family GPU; the
/// format converters resolve 24-bit depth to Depth32Float_Stencil8 instead,
/// and this guard catches any path that still asks for it.
bool metal_pixel_format_supported(id<MTLDevice> device, u32 pixel_format) {
    if (pixel_format == 0) return false;
    if (pixel_format == 255 /* Depth24Unorm_Stencil8 */ &&
        (device == nil || !device.depth24Stencil8PixelFormatSupported)) {
        return false;
    }
    return true;
}

/// The IOSurface value a shared texture's SharedTextureRecord lives under.
CFStringRef shared_record_key() { return CFSTR("VitreoSharedTexture"); }

/// CoreVideo pixel-format code whose documented layout (CVPixelBuffer.h) is
/// exactly this Metal format's, or 0. The code is descriptive only -- every
/// Vitreo opener reads the SharedTextureRecord -- so a format without an
/// exact CoreVideo twin carries none rather than a near miss: 'l10r' is
/// ARGB2101010 with R in bits 20-29, the reverse of RGB10A2Unorm.
u32 core_video_code(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB: return 0x42475241u;   // 'BGRA'
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB: return 0x52474241u;   // 'RGBA'
    case MTLPixelFormatRGBA16Float: return 0x52476841u;       // 'RGhA'
    case MTLPixelFormatRGBA32Float: return 0x52476641u;       // 'RGfA'
    case MTLPixelFormatRGBA16Unorm: return 0x6c363472u;       // 'l64r'
    case MTLPixelFormatR8Unorm: return 0x4c303038u;           // 'L008'
    case MTLPixelFormatRG8Unorm: return 0x32433038u;          // '2C08'
    case MTLPixelFormatR16Unorm: return 0x4c303136u;          // 'L016'
    case MTLPixelFormatRG16Unorm: return 0x32433136u;         // '2C16'
    case MTLPixelFormatR16Float: return 0x4c303068u;          // 'L00h'
    case MTLPixelFormatRG16Float: return 0x32433068u;         // '2C0h'
    case MTLPixelFormatR32Float: return 0x4c303066u;          // 'L00f'
    case MTLPixelFormatRG32Float: return 0x32433066u;         // '2C0f'
    default: return 0u;
    }
}

} // namespace

u32 iosurface_bytes_per_element(u32 mtl_pixel_format) noexcept {
    // Ordinary uncompressed colour formats: Metal backs these with an
    // IOSurface whose element size is the texel size. Compressed, depth and
    // stencil formats have no IOSurface form, and the packed 16-bit /
    // shared-exponent ones are left out (not IOSurface-renderable on every
    // Mac GPU).
    switch (static_cast<MTLPixelFormat>(mtl_pixel_format)) {
    case MTLPixelFormatA8Unorm:
    case MTLPixelFormatR8Unorm:
    case MTLPixelFormatR8Snorm:
    case MTLPixelFormatR8Uint:
    case MTLPixelFormatR8Sint:
        return 1;
    case MTLPixelFormatR16Unorm:
    case MTLPixelFormatR16Snorm:
    case MTLPixelFormatR16Uint:
    case MTLPixelFormatR16Sint:
    case MTLPixelFormatR16Float:
    case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatRG8Snorm:
    case MTLPixelFormatRG8Uint:
    case MTLPixelFormatRG8Sint:
        return 2;
    case MTLPixelFormatR32Uint:
    case MTLPixelFormatR32Sint:
    case MTLPixelFormatR32Float:
    case MTLPixelFormatRG16Unorm:
    case MTLPixelFormatRG16Snorm:
    case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRG16Sint:
    case MTLPixelFormatRG16Float:
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGB10A2Unorm:
    case MTLPixelFormatRGB10A2Uint:
    case MTLPixelFormatRG11B10Float:
        return 4;
    case MTLPixelFormatRG32Uint:
    case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm:
    case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatRGBA16Float:
        return 8;
    case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatRGBA32Sint:
    case MTLPixelFormatRGBA32Float:
        return 16;
    default:
        return 0;
    }
}

/// The Metal descriptor for @p desc, or nil when it cannot be expressed.
/// Shared by texture creation and by the placement-heap size query
/// (placement_size_and_align), so a texture placed in a heap is exactly the one its
/// size and alignment were computed for.
static MTLTextureDescriptor* texture_descriptor_for(const TextureDesc& desc) {
    MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
    td.textureType      = static_cast<MTLTextureType>(desc.texture_type);
    td.pixelFormat       = static_cast<MTLPixelFormat>(desc.pixel_format);
    td.width             = desc.width;
    td.height            = desc.height;
    td.depth             = desc.depth;
    td.mipmapLevelCount  = desc.mip_levels;
    td.arrayLength       = desc.array_size;
    td.sampleCount       = desc.sample_count > 1 ? desc.sample_count : 1;
    if (desc.sample_count > 1) {
        td.textureType = desc.array_size > 1
            ? MTLTextureType2DMultisampleArray
            : MTLTextureType2DMultisample;
        td.mipmapLevelCount = 1;
        td.arrayLength = desc.array_size;
    }
    td.storageMode       = static_cast<MTLStorageMode>(desc.storage_mode);

    MTLTextureUsage mtl_usage = static_cast<MTLTextureUsage>(desc.usage);
    if (desc.render_target) {
        mtl_usage |= MTLTextureUsageRenderTarget;
    }
    // Learn D3D11_TEX2D_SRV: an SRV may expose a mip window smaller than
    // the resource. Metal requires PixelFormatView to create that view.
    if ((mtl_usage & MTLTextureUsageShaderRead) != 0 &&
        (desc.mip_levels > 1 || desc.array_size > 1)) {
        mtl_usage |= MTLTextureUsagePixelFormatView;
    }
    td.usage = mtl_usage;
    if (desc.placement_sparse) {
        if (@available(macOS 26.0, *)) {
            td.placementSparsePageSize = MTLSparsePageSize64;
            td.storageMode = MTLStorageModePrivate;
            // Lossless compression keeps per-texture metadata outside the
            // mapped pages, so another tiled resource sharing those pages
            // would read the compressed bytes as texels (measured: a render
            // pass on the reading texture scrambles an alias's data, 256 of
            // 65536 texels right). D3D resources sharing tiles see the same
            // data when compatible (Learn, "Tile access limitations with
            // duplicate mappings"), so tiled textures keep plain storage.
            td.allowGPUOptimizedContents = NO;
        } else {
            return nil;
        }
    }

    // D3D9 L8/A8L8 sample as luminance RGB (Learn: undefined channels → 1).
    // Metal R8/RG8 otherwise return (R,0,0,1)/(R,G,0,1).
    if (desc.channel_swizzle == 1u) {
        td.swizzle = MTLTextureSwizzleChannelsMake(
            MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed,
            MTLTextureSwizzleOne);
    } else if (desc.channel_swizzle == 2u) {
        td.swizzle = MTLTextureSwizzleChannelsMake(
            MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed,
            MTLTextureSwizzleGreen);
    }
    if (desc.heap_placed) {
        // A texture placed in a D3D12 heap (CreatePlacedResource) takes the
        // heap's storage and hazard tracking -- a GPU-private placement heap
        // is Private and tracked (MetalBackendHeap), which is also what a
        // size query without a heap assumes. It keeps plain (uncompressed)
        // contents: lossless compression keeps metadata outside the texture's
        // bytes, so an aliased resource written over the same memory would
        // leave it stale (the reason tiled textures do the same, above), and
        // D3D12 aliasing is a contract (Learn, "Memory aliasing and data
        // inheritance").
        td.storageMode = MTLStorageModePrivate;
        td.hazardTrackingMode = MTLHazardTrackingModeTracked;
        if (desc.placement_heap != nullptr) {
            id<MTLHeap> heap = (__bridge id<MTLHeap>)desc.placement_heap;
            td.storageMode = heap.storageMode;
            td.cpuCacheMode = heap.cpuCacheMode;
            td.hazardTrackingMode = heap.hazardTrackingMode;
        }
        td.allowGPUOptimizedContents = NO;
    }
    return td;
}

bool MetalTexture::placement_size_and_align(void* device,
                                            const TextureDesc& desc,
                                            u64& size, u64& alignment) {
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
    if (mtl_device == nil || !desc.heap_placed ||
        !metal_pixel_format_supported(mtl_device, desc.pixel_format)) {
        return false;
    }
    MTLTextureDescriptor* td = texture_descriptor_for(desc);
    if (td == nil) return false;
    const MTLSizeAndAlign placement =
        [mtl_device heapTextureSizeAndAlignWithDescriptor:td];
    if (placement.size == 0) return false;
    size = placement.size;
    alignment = placement.align;
    return true;
}

MetalTexture::MetalTexture(void* device, const TextureDesc& desc,
                           const void* initial_data)
    : m_desc(desc)
{
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;

    if (!metal_pixel_format_supported(mtl_device, desc.pixel_format)) {
        VITREO_TRACE("[texture] unsupported pixel format %u for %ux%u\n",
                     desc.pixel_format, desc.width, desc.height);
        return;
    }

    MTLTextureDescriptor* td = texture_descriptor_for(desc);
    if (td == nil) return;
    if (desc.placement_sparse) m_desc.storage_mode = MTLStorageModePrivate;
    if (desc.heap_placed) {
        if (desc.placement_heap == nullptr) return;
        id<MTLHeap> heap = (__bridge id<MTLHeap>)desc.placement_heap;
        m_desc.storage_mode = static_cast<u32>(heap.storageMode);
        id<MTLTexture> placed = [heap newTextureWithDescriptor:td
                                                        offset:desc.heap_offset];
        m_texture = (__bridge_retained void*)placed;
        return;
    }

    id<MTLTexture> tex = nil;
    // Learn D3D11_RESOURCE_MISC_SHARED: the flag says the resource *may* be
    // shared with another device, not that it must be. Vitreo backs a shared
    // resource with a global IOSurface so a peer host process can open it by
    // ID, and an IOSurface holds exactly one 2D image of an ordinary colour
    // format (iosurface_bytes_per_element). Every other shape still has to be
    // a working texture: failing creation turns an optional capability into a
    // fatal render-thread error. A D3D11 renderer tags ordinary render targets
    // D3D11_RESOURCE_MISC_SHARED and treats a failed CreateTexture2D as
    // unrecoverable. Such a texture is created here without an IOSurface and
    // shared through a canonical store (metal_shared_resource.hpp,
    // MetalBackendTexture2D).
    const u32 element_bytes = iosurface_bytes_per_element(desc.pixel_format);
    const bool iosurface_backed =
        desc.shareable && !desc.placement_sparse && desc.texture_type == 2u && desc.mip_levels == 1u &&
        desc.array_size == 1u && desc.depth == 1u && desc.sample_count <= 1u &&
        element_bytes != 0u;
    if (desc.shareable && !iosurface_backed) {
        VITREO_TRACE(
            "[texture] shareable %ux%u fmt=%u mips=%u array=%u type=%u has no "
            "IOSurface representation; shared through a store\n",
            desc.width, desc.height, desc.pixel_format, desc.mip_levels,
            desc.array_size, desc.texture_type);
    }
    if (iosurface_backed) {
        // Metal reads an IOSurface-backed texture linearly: its row pitch must
        // be a multiple of the device's linear-texture alignment for the
        // format (at least 16 bytes, which narrow surfaces such as the 1x1
        // synchronisation textures graphics clients share also need), and of
        // what IOSurface itself aligns rows to.
        usize align = std::max<usize>(
            16u, [mtl_device minimumLinearTextureAlignmentForPixelFormat:
                                 static_cast<MTLPixelFormat>(
                                     desc.pixel_format)]);
        usize bytes_per_row =
            (static_cast<usize>(desc.width) * element_bytes + align - 1u) /
            align * align;
        bytes_per_row = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow,
                                               bytes_per_row);
        NSMutableDictionary* properties = [@{
            (__bridge NSString*)kIOSurfaceWidth : @(desc.width),
            (__bridge NSString*)kIOSurfaceHeight : @(desc.height),
            (__bridge NSString*)kIOSurfaceBytesPerElement : @(element_bytes),
            (__bridge NSString*)kIOSurfaceBytesPerRow : @(bytes_per_row),
            (__bridge NSString*)kIOSurfaceAllocSize :
                @(bytes_per_row * static_cast<usize>(desc.height)),
            // Backend shared handles are IOSurface IDs. Mark the surface
            // global so a peer Vitreo host process can resolve that ID and
            // create its own Metal texture view without copying pixels.
            (__bridge NSString*)kIOSurfaceIsGlobal : @YES,
        } mutableCopy];
        const u32 code = core_video_code(
            static_cast<MTLPixelFormat>(desc.pixel_format));
        if (code != 0u) {
            properties[(__bridge NSString*)kIOSurfacePixelFormat] = @(code);
        }
        IOSurfaceRef surface = IOSurfaceCreate(
            (__bridge CFDictionaryRef)properties);
        if (surface != nullptr) {
            // The record's Metal fields are the texture actually allocated;
            // the rest (backend and frontend descriptions) is the caller's.
            SharedTextureRecord record{};
            if (!desc.shared_record.empty()) {
                (void)SharedTextureRecord::decode(desc.shared_record.data(),
                                                  desc.shared_record.size(),
                                                  record);
            }
            record.mtl_pixel_format = desc.pixel_format;
            record.usage = static_cast<u32>(td.usage);
            const std::vector<u8> encoded = record.encode();
            CFDataRef record_data = CFDataCreate(
                kCFAllocatorDefault, encoded.data(),
                static_cast<CFIndex>(encoded.size()));
            if (record_data != nullptr) {
                IOSurfaceSetValue(surface, shared_record_key(), record_data);
                CFRelease(record_data);
                MTLStorageMode requested = td.storageMode;
                td.storageMode = MTLStorageModeShared;
                tex = [mtl_device newTextureWithDescriptor:td
                                                 iosurface:surface
                                                     plane:0];
                if (tex == nil) td.storageMode = requested;
            }
            if (tex != nil) {
                m_iosurface = surface;
                m_desc.storage_mode = MTLStorageModeShared;
                m_desc.shared_record = encoded;
            } else {
                CFRelease(surface);
            }
        }
        if (tex == nil) {
            // No IOSurface for it (e.g. beyond what IOSurface allocates):
            // still a working texture, reporting no shared handle.
            VITREO_TRACE("[texture] shareable %ux%u fmt=%u: IOSurface "
                         "allocation failed; created unshared\n",
                         desc.width, desc.height, desc.pixel_format);
        }
    }
    if (tex == nil) {
        m_desc.shared_record.clear();
        tex = [mtl_device newTextureWithDescriptor:td];
    }
    m_texture = (__bridge_retained void*)tex;

    VITREO_TRACE("[texture] created %ux%u fmt=%u\n",
                 desc.width, desc.height, desc.pixel_format);

    if (initial_data) {
        // Compute bytes-per-row: assume 4 bytes/pixel for common BGRA8.
        // Caller should use upload() directly for non-standard formats.
        usize bpr = static_cast<usize>(desc.width) * 4;
        upload(initial_data, bpr, 0, 0);
    }
}

MetalTexture::MetalTexture(void* device, u32 iosurface_id) {
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
    IOSurfaceRef surface = IOSurfaceLookup(iosurface_id);
    if (mtl_device == nil || surface == nullptr) {
        if (std::getenv("VITREO_D3D11_DIAG") != nullptr) {
            std::fprintf(stderr,
                         "[texture] open_shared IOSurfaceLookup id=%u "
                         "device=%p surface=%p\n",
                         iosurface_id, device, surface);
        }
        if (surface != nullptr) CFRelease(surface);
        return;
    }
    const u32 width = static_cast<u32>(IOSurfaceGetWidth(surface));
    const u32 height = static_cast<u32>(IOSurfaceGetHeight(surface));
    // The creator's record names the Metal texture to recreate. A surface
    // without one (not a Vitreo shared texture) opens only as the 32-bit
    // BGRA form the IOSurface says it is.
    SharedTextureRecord record{};
    bool has_record = false;
    if (CFTypeRef value = IOSurfaceCopyValue(surface, shared_record_key())) {
        if (CFGetTypeID(value) == CFDataGetTypeID()) {
            CFDataRef data = static_cast<CFDataRef>(value);
            has_record = SharedTextureRecord::decode(
                CFDataGetBytePtr(data),
                static_cast<usize>(CFDataGetLength(data)), record);
        }
        CFRelease(value);
    }
    if (!has_record) {
        record = {};
        record.mtl_pixel_format = MTLPixelFormatBGRA8Unorm;
        record.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    }
    const u32 element_bytes =
        iosurface_bytes_per_element(record.mtl_pixel_format);
    const bool legacy_ok =
        has_record || IOSurfaceGetPixelFormat(surface) == 0x42475241u;
    if (width == 0 || height == 0 || !legacy_ok || element_bytes == 0 ||
        IOSurfaceGetBytesPerElement(surface) != element_bytes ||
        IOSurfaceGetPlaneCount(surface) > 1) {
        if (std::getenv("VITREO_D3D11_DIAG") != nullptr) {
            std::fprintf(stderr,
                         "[texture] open_shared reject id=%u %ux%u fmt=0x%x "
                         "record=%d mtl=%u\n",
                         iosurface_id, width, height,
                         static_cast<unsigned>(IOSurfaceGetPixelFormat(surface)),
                         has_record ? 1 : 0, record.mtl_pixel_format);
        }
        CFRelease(surface);
        return;
    }

    MTLTextureDescriptor* td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:static_cast<MTLPixelFormat>(
                                               record.mtl_pixel_format)
                                     width:width
                                    height:height
                                 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = static_cast<MTLTextureUsage>(record.usage);
    id<MTLTexture> tex = [mtl_device newTextureWithDescriptor:td
                                         iosurface:surface
                                             plane:0];
    if (tex == nil) {
        CFRelease(surface);
        return;
    }
    m_desc.width = width;
    m_desc.height = height;
    m_desc.pixel_format = record.mtl_pixel_format;
    m_desc.texture_type = 2u;
    m_desc.mip_levels = 1u;
    m_desc.array_size = 1u;
    m_desc.storage_mode = MTLStorageModeShared;
    m_desc.usage = record.usage;
    m_desc.render_target = (record.usage & MTLTextureUsageRenderTarget) != 0;
    m_desc.shareable = true;
    if (has_record) m_desc.shared_record = record.encode();
    m_iosurface = surface;
    m_texture = (__bridge_retained void*)tex;
}

MetalTexture::MetalTexture(void* device, void* buffer, usize offset,
                           usize bytes_per_row, const TextureDesc& desc)
    : m_desc(desc)
{
    (void)device;  // the texture inherits the buffer's device
    id<MTLBuffer> mtl_buffer = (__bridge id<MTLBuffer>)buffer;
    if (mtl_buffer == nil) return;

    // Metal only backs 2D, single-mip, non-array textures with a buffer.
    // Silently producing a texture with its own storage instead would break
    // the aliasing the caller is relying on, so refuse and report null.
    if (desc.mip_levels != 1 || desc.array_size != 1 || desc.depth != 1) {
        VITREO_TRACE("[texture] buffer-backed requires 2D/1 mip/1 slice "
                     "(mips=%u slices=%u depth=%u)\n",
                     desc.mip_levels, desc.array_size, desc.depth);
        return;
    }
    if (bytes_per_row == 0 || offset + bytes_per_row * desc.height > [mtl_buffer length]) {
        VITREO_TRACE("[texture] buffer-backed range exceeds buffer: "
                     "offset=%zu bpr=%zu height=%u len=%zu\n",
                     offset, bytes_per_row, desc.height,
                     static_cast<usize>([mtl_buffer length]));
        return;
    }

    if (!metal_pixel_format_supported([mtl_buffer device], desc.pixel_format)) {
        VITREO_TRACE("[texture] buffer-backed unsupported pixel format %u\n",
                     desc.pixel_format);
        return;
    }

    MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
    td.textureType      = MTLTextureType2D;
    td.pixelFormat      = static_cast<MTLPixelFormat>(desc.pixel_format);
    td.width            = desc.width;
    td.height           = desc.height;
    td.depth            = 1;
    td.mipmapLevelCount = 1;
    td.arrayLength      = 1;
    // A buffer-backed texture must share the buffer's storage mode; the
    // allocations these alias are created shared so the CPU can write them.
    td.storageMode      = MTLStorageModeShared;

    MTLTextureUsage mtl_usage = static_cast<MTLTextureUsage>(desc.usage);
    if (desc.render_target) mtl_usage |= MTLTextureUsageRenderTarget;
    td.usage = mtl_usage;

    id<MTLTexture> tex = [mtl_buffer newTextureWithDescriptor:td
                                                       offset:offset
                                                  bytesPerRow:bytes_per_row];
    m_texture = (__bridge_retained void*)tex;

    VITREO_TRACE("[texture] buffer-backed %ux%u fmt=%u offset=%zu bpr=%zu -> %s\n",
                 desc.width, desc.height, desc.pixel_format, offset, bytes_per_row,
                 tex ? "ok" : "FAILED");
}

MetalTexture::~MetalTexture() {
    if (m_texture && m_desc.placement_sparse) {
        MetalTiled::instance().forget(m_texture);
    }
    if (m_texture) {
        CFRelease(m_texture);
        m_texture = nullptr;
    }
    if (m_iosurface) {
        CFRelease(static_cast<IOSurfaceRef>(m_iosurface));
        m_iosurface = nullptr;
    }
}

u32 MetalTexture::shared_handle() const {
    return m_iosurface != nullptr
        ? IOSurfaceGetID(static_cast<IOSurfaceRef>(m_iosurface))
        : 0u;
}

void MetalTexture::upload(const void* data, usize bytes_per_row,
                          u32 mip, u32 slice) {
    u32 h = m_desc.height >> mip;
    if (h == 0) h = 1;
    upload_rows(data, bytes_per_row, mip, slice, 0, h);
}

void MetalTexture::upload_rows(const void* data, usize bytes_per_row,
                               u32 mip, u32 slice, u32 row_y, u32 row_height) {
    id<MTLTexture> tex = (__bridge id<MTLTexture>)m_texture;
    if (tex == nil || data == nullptr || bytes_per_row == 0 || row_height == 0) {
        return;
    }

    u32 w = m_desc.width >> mip;
    u32 h = m_desc.height >> mip;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (row_y >= h) return;
    if (row_y + row_height > h) {
        row_height = h - row_y;
    }

    // Compressed BC uploads are full-level only (block-row alignment).
    const u32 block_bytes = compressed_block_bytes_mtl(m_desc.pixel_format);
    if (block_bytes != 0) {
        if (row_y != 0 || row_height != h) return;
        MTLRegion region = MTLRegionMake2D(0, 0, w, h);
        const usize row_count = static_cast<usize>((h + 3u) / 4u);
        const usize bytes_per_image = bytes_per_row * row_count;
        [tex replaceRegion:region
               mipmapLevel:mip
                     slice:slice
                 withBytes:data
               bytesPerRow:bytes_per_row
             bytesPerImage:bytes_per_image];
        return;
    }

    MTLRegion region = MTLRegionMake2D(0, row_y, w, row_height);
    const usize bytes_per_image = bytes_per_row * row_height;
    [tex replaceRegion:region
           mipmapLevel:mip
                 slice:slice
             withBytes:data
           bytesPerRow:bytes_per_row
           bytesPerImage:bytes_per_image];
}

void MetalTexture::upload_region(const void* data, usize bytes_per_row,
                                 u32 mip, u32 slice, u32 x, u32 y,
                                 u32 width, u32 height) {
    id<MTLTexture> tex = (__bridge id<MTLTexture>)m_texture;
    if (tex == nil || data == nullptr || bytes_per_row == 0 ||
        width == 0 || height == 0) {
        return;
    }
    const u32 level_width = std::max(1u, m_desc.width >> mip);
    const u32 level_height = std::max(1u, m_desc.height >> mip);
    if (x >= level_width || y >= level_height ||
        width > level_width - x || height > level_height - y ||
        compressed_block_bytes_mtl(m_desc.pixel_format) != 0) {
        return;
    }
    const MTLRegion region = MTLRegionMake2D(x, y, width, height);
    [tex replaceRegion:region
           mipmapLevel:mip
                 slice:slice
             withBytes:data
           bytesPerRow:bytes_per_row
         bytesPerImage:bytes_per_row * height];
}

void MetalTexture::upload_volume(const void* data, usize bytes_per_row,
                                 usize bytes_per_image, u32 mip) {
    id<MTLTexture> tex = (__bridge id<MTLTexture>)m_texture;
    if (tex == nil || data == nullptr || bytes_per_row == 0 ||
        bytes_per_image == 0) {
        return;
    }
    if (m_desc.texture_type != 7u /* MTLTextureType3D */) return;

    u32 w = m_desc.width >> mip;
    u32 h = m_desc.height >> mip;
    u32 d = m_desc.depth >> mip;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (d == 0) d = 1;

    // Compressed volume mips are full-level only.
    const u32 block_bytes = compressed_block_bytes_mtl(m_desc.pixel_format);
    if (block_bytes != 0) {
        const usize row_count = static_cast<usize>((h + 3u) / 4u);
        if (bytes_per_image < bytes_per_row * row_count) return;
    } else if (bytes_per_image < bytes_per_row * h) {
        return;
    }

    MTLRegion region = MTLRegionMake3D(0, 0, 0, w, h, d);
    [tex replaceRegion:region
           mipmapLevel:mip
                 slice:0
             withBytes:data
           bytesPerRow:bytes_per_row
         bytesPerImage:bytes_per_image];
}

} // namespace vitreo::graphics
