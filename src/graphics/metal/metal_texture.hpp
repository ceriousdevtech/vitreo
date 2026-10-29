// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_texture.hpp
/// @brief Metal GPU texture wrapper

#include <vitreo/common.hpp>

#include <cstring>
#include <vector>

namespace vitreo::graphics {

/// What a shared texture's IOSurface carries besides its pixels (the
/// IOSurface value "VitreoSharedTexture", which IOSurface keeps with the
/// surface for every process that looks it up): the Metal texture each opener
/// recreates over the same memory, the backend description it came from, and
/// the creating API's own description, opaque here.
struct SharedTextureRecord {
    static constexpr u32 kMagic = 0x58545356u;  // 'VSTX'
    static constexpr u32 kVersion = 1u;
    u32 mtl_pixel_format{0};
    u32 usage{0};            ///< MTLTextureUsage the creator allocated with
    u32 backend_format{0};   ///< graphics::PixelFormat
    u32 backend_flags{0};    ///< kRenderTarget | kShaderResource | ...
    std::vector<u8> frontend;

    static constexpr u32 kRenderTarget = 1u;
    static constexpr u32 kShaderResource = 2u;
    static constexpr u32 kUnorderedAccess = 4u;
    static constexpr u32 kPixelFormatView = 8u;

    [[nodiscard]] std::vector<u8> encode() const {
        const u32 header[7] = {kMagic, kVersion, mtl_pixel_format, usage,
                               backend_format, backend_flags,
                               static_cast<u32>(frontend.size())};
        std::vector<u8> bytes(sizeof(header) + frontend.size());
        std::memcpy(bytes.data(), header, sizeof(header));
        if (!frontend.empty()) {
            std::memcpy(bytes.data() + sizeof(header), frontend.data(),
                        frontend.size());
        }
        return bytes;
    }

    [[nodiscard]] static bool decode(const u8* bytes, usize size,
                                     SharedTextureRecord& out) {
        u32 header[7] = {};
        if (bytes == nullptr || size < sizeof(header)) return false;
        std::memcpy(header, bytes, sizeof(header));
        if (header[0] != kMagic || header[1] != kVersion ||
            size != sizeof(header) + header[6]) {
            return false;
        }
        out.mtl_pixel_format = header[2];
        out.usage = header[3];
        out.backend_format = header[4];
        out.backend_flags = header[5];
        out.frontend.assign(bytes + sizeof(header), bytes + size);
        return true;
    }
};

/// Bytes per element of the IOSurface that backs a shared texture of
/// @p mtl_pixel_format, or 0 when that format has no IOSurface form
/// (compressed, depth/stencil, packed 16-bit and shared-exponent formats).
[[nodiscard]] u32 iosurface_bytes_per_element(u32 mtl_pixel_format) noexcept;

struct TextureDesc {
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mip_levels = 1;
    u32 array_size = 1;
    u32 pixel_format = 80;    // MTLPixelFormatBGRA8Unorm
    u32 texture_type = 2;     // MTLTextureType2D
    u32 usage = 0x01;         // MTLTextureUsageShaderRead
    u32 storage_mode = 2;     // MTLStorageModePrivate
    u32 sample_count = 1;
    bool render_target = false;
    bool shareable = false;
    /// 0 = identity, 1 = L8 (RGB←R,A←1), 2 = A8L8 (RGB←R,A←G).
    u32 channel_swizzle = 0;
    /// A shareable texture's SharedTextureRecord (encoded), stored with its
    /// IOSurface; for an opened one, the record read back.
    std::vector<u8> shared_record;
    /// A placement sparse texture with 64 KiB pages (metal_tiled.hpp): no
    /// memory of its own, Private storage, never shared.
    bool placement_sparse = false;
    /// Place the texture in `placement_heap` (an id<MTLHeap> of type
    /// placement) at `heap_offset` instead of allocating storage (D3D12
    /// placed resources). The texture takes the heap's storage mode; never
    /// shared. `heap_placed` without a heap describes such a texture for a
    /// size query only (placement_size_and_align).
    bool heap_placed = false;
    void* placement_heap = nullptr;
    u64 heap_offset = 0;
};

class MetalTexture {
public:
    MetalTexture(void* device, const TextureDesc& desc,
                 const void* initial_data = nullptr);
    /// Open the shared texture whose IOSurface has global ID @p iosurface_id:
    /// a Metal texture over the same memory, recreated from the surface's
    /// SharedTextureRecord (a surface without one opens only as the legacy
    /// 32-bit BGRA form). handle() is null when the ID names no surface.
    MetalTexture(void* device, u32 iosurface_id);

    /// Create a texture whose pixels live inside an existing MTLBuffer, at
    /// `offset`, with the given row stride.
    ///
    /// This is how a linear-tiled image aliases memory the guest has mapped:
    /// pixels written through the mapping are the pixels the GPU samples, with
    /// no copy. A texture with its own storage cannot do that, so an API whose
    /// contract says "map this image and write to it" needs this form.
    ///
    /// Metal restricts buffer-backed textures to 2D, single-mip, non-array,
    /// and requires `bytes_per_row` to satisfy the device's alignment for the
    /// format. handle() is null when the constraints are not met.
    MetalTexture(void* device, void* buffer, usize offset, usize bytes_per_row,
                 const TextureDesc& desc);

    ~MetalTexture();

    /// Bytes and alignment the texture @p desc (with `heap_placed` set)
    /// needs in a placement heap: heapTextureSizeAndAlignWithDescriptor of
    /// the descriptor creation would use.
    [[nodiscard]] static bool placement_size_and_align(void* device,
                                                       const TextureDesc& desc,
                                                       u64& size,
                                                       u64& alignment);

    void* handle() const { return m_texture; }  // id<MTLTexture>
    const TextureDesc& desc() const { return m_desc; }
    u32 shared_handle() const;

    void upload(const void* data, usize bytes_per_row,
                u32 mip = 0, u32 slice = 0);

    /// Upload a contiguous row range `[row_y, row_y + row_height)`.
    void upload_rows(const void* data, usize bytes_per_row,
                     u32 mip, u32 slice, u32 row_y, u32 row_height);

    /// Upload an uncompressed texel rectangle.
    void upload_region(const void* data, usize bytes_per_row,
                       u32 mip, u32 slice, u32 x, u32 y,
                       u32 width, u32 height);

    /// Upload one mip of a 3D texture (`MTLTextureType3D`).
    /// `bytes_per_image` is the guest slice pitch (bytes between depth planes).
    void upload_volume(const void* data, usize bytes_per_row,
                       usize bytes_per_image, u32 mip = 0);

    MetalTexture(const MetalTexture&) = delete;
    MetalTexture& operator=(const MetalTexture&) = delete;

private:
    void* m_texture = nullptr;
    void* m_iosurface = nullptr;
    TextureDesc m_desc;
};

} // namespace vitreo::graphics
