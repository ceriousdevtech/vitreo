// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_backend_texture_2d.hpp
/// @brief The backend 2D texture
///
/// Private to vitreo_graphics_metal, split out of metal_device.mm: included
/// only by that target's .mm files, after <Metal/Metal.h> (the layering
/// check keeps Apple framework imports out of headers).

#include <vitreo/ab_cycle.hpp>
#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/metal/metal_backend_enums.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_texture.hpp"

#include <array>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace vitreo::graphics {

class MetalBackendTexture2D final : public BackendTexture2D {
public:
    [[nodiscard]] static const char* describe(const BackendTexture2DDesc& desc,
                                              TextureDesc& metal_desc,
                                              u32& cube_faces);

    MetalBackendTexture2D(void* device, void* command_queue,
                          BackendTexture2DDesc desc,
                          const void* initial_data,
                          unsigned initial_row_pitch);

    void zero_initialize(const FormatInfo& format);

    MetalBackendTexture2D(void* device, void* command_queue,
                          std::shared_ptr<MetalSharedLocal> local,
                          BackendTexture2DDesc desc);

    /// A wrapper over an existing plane texture (a planar texture's chroma
    /// plane shared with another opening).
    MetalBackendTexture2D(void* device, void* command_queue,
                          std::shared_ptr<MetalTexture> texture,
                          BackendTexture2DDesc desc)
        : m_desc(std::move(desc))
        , m_command_queue(command_queue)
        , m_texture(std::move(texture)) {
        m_device = device;
        if (m_texture) m_metal_desc = m_texture->desc();
    }

    [[nodiscard]] static SharedStoreRecord shared_record_for(
        const BackendTexture2DDesc& desc);

    [[nodiscard]] static BackendTexture2DDesc desc_from_record(
        const SharedStoreRecord& record);

    /// The mirrored-sharing state, if this texture has one.
    [[nodiscard]] const std::shared_ptr<MetalSharedLocal>& shared_local()
        const noexcept {
        return m_shared;
    }
    [[nodiscard]] const std::shared_ptr<MetalTexture>& local_texture()
        const noexcept {
        return m_texture;
    }
    /// A planar texture's chroma-plane MetalTexture (null otherwise).
    [[nodiscard]] std::shared_ptr<MetalTexture> local_chroma_texture()
        const noexcept {
        return m_plane1 ? m_plane1->m_texture : nullptr;
    }
    /// Adopt @p local (a store another process shares) as this texture's
    /// mirror: the texture was created from the store's record.
    void adopt_shared(std::shared_ptr<MetalSharedLocal> local) {
        m_shared = std::move(local);
        m_desc.shareable = true;
        m_desc.shared_metadata = m_shared->record().frontend;
    }

    MetalBackendTexture2D(void* device, void* command_queue,
                          u32 shared_handle);

    /// Borrow an existing Metal texture (swap-chain MSAA / resolve target).
    /// Does not take ownership of @p borrowed_handle.
    MetalBackendTexture2D(void* borrowed_handle, void* command_queue,
                          BackendTexture2DDesc desc)
        : m_desc(desc)
        , m_command_queue(command_queue)
        , m_borrowed_handle(borrowed_handle) {}

    bool valid() const noexcept override {
        return native_handle() != nullptr;
    }

    const BackendTexture2DDesc& desc() const noexcept override {
        return m_desc;
    }

    u64 shared_handle() const noexcept override {
        if (m_texture != nullptr && m_texture->shared_handle() != 0) {
            return m_texture->shared_handle();
        }
        if (m_shared) {
            const auto store = m_shared->store();
            return store ? store->id() : 0u;
        }
        return 0u;
    }

    u64 export_shared() override {
        if (m_texture != nullptr && m_texture->shared_handle() != 0) {
            return m_texture->shared_handle();
        }
        return m_shared ? m_shared->export_store() : 0u;
    }

    const std::vector<u8>& shared_metadata() const noexcept override {
        return m_desc.shared_metadata;
    }

    bool clear_render_target(
        const std::array<float, 4>& color) override;

    bool clear_render_target_level(
        const std::array<float, 4>& color,
        unsigned mip_level, unsigned first_array_slice = 0,
        unsigned array_size = 1) override;

    bool clear_render_target_view(const std::array<float, 4>& color,
                                  PixelFormat view_format, unsigned mip_level,
                                  unsigned first_array_slice,
                                  unsigned array_size) override;

    bool clear_render_target_through(void* attachment,
                                     const std::array<float, 4>& color,
                                     unsigned mip_level,
                                     unsigned first_array_slice,
                                     unsigned array_size);

    bool clear_depth_stencil(float depth, u8 stencil) override;

    bool clear_depth_stencil_level(
        float depth, u8 stencil, unsigned mip_level,
        unsigned first_array_slice, unsigned array_size) override;

    bool clear_depth_stencil_aspects(
        float depth, u8 stencil, bool clear_depth, bool clear_stencil,
        unsigned mip_level, unsigned first_array_slice,
        unsigned array_size) override;

    bool update(unsigned mip_level, const void* data,
                unsigned row_pitch) override;

    bool update_slice(unsigned mip_level, unsigned slice, const void* data,
                      unsigned row_pitch) override;

    bool update_plane_slice(unsigned mip_level, unsigned slice,
                            const void* data, unsigned row_pitch);

    bool readback(unsigned mip_level, void* data,
                  unsigned row_pitch) override;

    bool readback_slice(unsigned mip_level, unsigned slice, void* data,
                        unsigned row_pitch) override;

    bool readback_plane_slice(unsigned mip_level, unsigned slice, void* data,
                              unsigned row_pitch);

    bool update_depth_stencil_plane(unsigned mip_level, unsigned slice,
                                    bool stencil, const void* data,
                                    unsigned row_pitch) override;

    std::shared_ptr<BackendReadback> readback_slice_async(
        unsigned mip_level, unsigned slice) override;

    std::shared_ptr<BackendReadback> readback_plane_slice_async(
        unsigned mip_level, unsigned slice);

    bool readback_depth_stencil_plane(unsigned mip_level, unsigned slice,
                                      bool stencil, void* data,
                                      unsigned row_pitch) override;

    bool blit_to_host(unsigned mip_level, unsigned slice,
                      usize bytes_per_texel, MTLBlitOption options,
                      void* data, unsigned row_pitch, u32 block_bytes = 0);

    bool update_rows(unsigned mip_level, const void* data,
                     unsigned row_pitch, unsigned y,
                     unsigned height) override;

    bool update_region(unsigned mip_level, unsigned slice, const void* data,
                       unsigned row_pitch, unsigned x, unsigned y,
                       unsigned width, unsigned height) override;

    bool update_plane_region(unsigned mip_level, unsigned slice,
                             const void* data, unsigned row_pitch,
                             unsigned x, unsigned y, unsigned width,
                             unsigned height);

    [[nodiscard]] void* native_handle() const noexcept {
        if (m_borrowed_handle != nullptr) return m_borrowed_handle;
        return m_texture != nullptr ? m_texture->handle() : nullptr;
    }

    bool discard_storage() override;

    bool fill_depth_stencil(bool stencil_plane, float depth, u8 stencil,
                            unsigned mip_level, unsigned first_array_slice,
                            unsigned array_size, unsigned x, unsigned y,
                            unsigned width, unsigned height) override;

    bool fill_texels(const void* texel, unsigned texel_bytes,
                     unsigned mip_level, unsigned first_array_slice,
                     unsigned array_size, unsigned x, unsigned y,
                     unsigned width, unsigned height) override;

    bool fill_depth_stencil_texels(u32 depth_bits, bool write_stencil,
                                   u8 stencil, unsigned mip_level,
                                   unsigned first_array_slice,
                                   unsigned array_size, unsigned x, unsigned y,
                                   unsigned width, unsigned height) override;

    /// A planar texture's views of the chroma plane: R8G8 (NV12) / R16G16
    /// or R32_UINT (P010 / P016) (Learn DXGI_FORMAT; measured
    /// d3d11_nv12_probe [views]).
    [[nodiscard]] bool chroma_view(PixelFormat view_format) const noexcept {
        if (!m_plane1) return false;
        switch (view_format) {
        case PixelFormat::RG8Unorm:
        case PixelFormat::RG8Uint:
            return m_desc.pixel_format == PixelFormat::NV12;
        case PixelFormat::RG16Unorm:
        case PixelFormat::RG16Uint:
        case PixelFormat::R32Uint:
            return m_desc.pixel_format == PixelFormat::P010;
        default:
            return false;
        }
    }
    [[nodiscard]] const MetalBackendTexture2D* chroma_plane() const noexcept {
        return m_plane1.get();
    }
    BackendTexture2D* view_plane(PixelFormat view_format) noexcept override {
        return chroma_view(view_format) ? m_plane1.get() : this;
    }
    [[nodiscard]] MetalBackendTexture2D* chroma_plane() noexcept {
        return m_plane1.get();
    }

    [[nodiscard]] void* native_render_target_handle(
        PixelFormat view_format) const {
        if (chroma_view(view_format)) {
            return m_plane1->native_render_target_handle(view_format);
        }
        if (view_format == PixelFormat::Unknown ||
            view_format == m_desc.pixel_format) {
            return native_handle();
        }
        BackendTextureViewDesc view{};
        view.pixel_format = view_format;
        view.mip_levels = m_desc.mip_levels;
        view.array_size = m_desc.array_size;
        return native_shader_handle(view, true);
    }

    /// Learn D3D11_TEX2D_SRV: MipLevels is the only mip window the shader
    /// may see. GENERATE_MIPS textures allocate a full chain (higher levels
    /// stay zero until GenerateMips). Binding that full MTLTexture lets
    /// SAMPLE_B lod bias read the empty levels even when the sampler's mip
    /// filter is NotMipmapped. A one-level view makes those mips unreachable.
    [[nodiscard]] void* native_shader_handle(
        const BackendTextureViewDesc& requested,
        bool mipmaps_enabled) const {
        if (chroma_view(requested.pixel_format)) {
            return m_plane1->native_shader_handle(requested, mipmaps_enabled);
        }
        void* parent = native_handle();
        if (parent == nullptr) return nullptr;
        if (m_view_memo.result != nullptr &&
            m_view_memo.matches(parent, requested, mipmaps_enabled) &&
            !diagnostics::ab_cycle_off("view-memo")) {
            return m_view_memo.result;
        }
        @autoreleasepool {
            id<MTLTexture> texture = (__bridge id<MTLTexture>)parent;
            NSUInteger texture_type = texture.textureType;
            switch (requested.dimension) {
            case BackendTextureViewDimension::Texture2D:
                texture_type = MTLTextureType2D;
                break;
            case BackendTextureViewDimension::Texture2DArray:
                texture_type = MTLTextureType2DArray;
                break;
            case BackendTextureViewDimension::Texture2DMS:
                texture_type = MTLTextureType2DMultisample;
                break;
            case BackendTextureViewDimension::Texture2DMSArray:
                texture_type = MTLTextureType2DMultisampleArray;
                break;
            case BackendTextureViewDimension::TextureCube:
                texture_type = MTLTextureTypeCube;
                break;
            case BackendTextureViewDimension::TextureCubeArray:
                texture_type = MTLTextureTypeCubeArray;
                break;
            case BackendTextureViewDimension::Automatic:
                break;
            }
            const NSUInteger mip_first = requested.most_detailed_mip;
            const NSUInteger mip_count = requested.mip_levels != 0
                ? requested.mip_levels
                : (mipmaps_enabled ? m_desc.mip_levels : 1u);
            const NSUInteger slice_first = requested.first_array_slice;
            const NSUInteger slice_count = requested.array_size != 0
                ? requested.array_size
                : (m_desc.cube ? m_desc.array_size : 1u);
            const FormatInfo requested_format =
                format_table(requested.pixel_format);
            MTLPixelFormat pixel_format =
                requested.pixel_format != PixelFormat::Unknown &&
                        requested_format.mtl_pixel_format != 0
                    ? static_cast<MTLPixelFormat>(
                          requested_format.mtl_pixel_format)
                    : texture.pixelFormat;
            // A stencil-plane view must match the storage's depth half: D24S8
            // is stored as Depth32Float_Stencil8 on Apple GPUs (no
            // Depth24Unorm_Stencil8), and an X24_Stencil8 view of it aborts
            // the process in Metal's validation on the first shader read
            // (d3d11_a08_formats_probe, X24_TYPELESS_G8_UINT).
            if (pixel_format == MTLPixelFormatX24_Stencil8 &&
                texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
                pixel_format = MTLPixelFormatX32_Stencil8;
            } else if (pixel_format == MTLPixelFormatX32_Stencil8 &&
                       texture.pixelFormat ==
                           MTLPixelFormatDepth24Unorm_Stencil8) {
                pixel_format = MTLPixelFormatX24_Stencil8;
            }
            const bool stencil_view =
                pixel_format == MTLPixelFormatX32_Stencil8 ||
                pixel_format == MTLPixelFormatX24_Stencil8;
            const BackendViewSwizzle requested_swizzle =
                stencil_view ? BackendViewSwizzle::Identity
                             : requested.swizzle;
            if (pixel_format == texture.pixelFormat &&
                texture_type == texture.textureType && mip_first == 0 &&
                mip_count == texture.mipmapLevelCount && slice_first == 0 &&
                slice_count == m_desc.array_size &&
                requested_swizzle == BackendViewSwizzle::Identity &&
                requested.component_mapping == 0) {
                m_view_memo.remember(parent, requested, mipmaps_enabled,
                                     parent);
                return parent;
            }
            const u64 key = static_cast<u64>(texture_type & 0xFu) |
                (static_cast<u64>(mip_first & 0xFFu) << 4u) |
                (static_cast<u64>(mip_count & 0xFFu) << 12u) |
                (static_cast<u64>(slice_first & 0xFFFu) << 20u) |
                (static_cast<u64>(slice_count & 0xFFFu) << 32u) |
                (static_cast<u64>(pixel_format & 0x3FFu) << 44u) |
                (static_cast<u64>(requested_swizzle) << 54u);
            const auto cached = m_shader_views.find(
                std::make_pair(key, requested.component_mapping));
            if (cached != m_shader_views.end()) {
                m_view_memo.remember(parent, requested, mipmaps_enabled,
                                     cached->second);
                return cached->second;
            }
            MTLTextureSwizzleChannels swizzle =
                MTLTextureSwizzleChannelsDefault;
            switch (requested_swizzle) {
            case BackendViewSwizzle::RGB1:
                swizzle = MTLTextureSwizzleChannelsMake(
                    MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                    MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
                break;
            case BackendViewSwizzle::GBAR:
                swizzle = MTLTextureSwizzleChannelsMake(
                    MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                    MTLTextureSwizzleAlpha, MTLTextureSwizzleRed);
                break;
            case BackendViewSwizzle::R001:
                swizzle = MTLTextureSwizzleChannelsMake(
                    MTLTextureSwizzleRed, MTLTextureSwizzleZero,
                    MTLTextureSwizzleZero, MTLTextureSwizzleOne);
                break;
            case BackendViewSwizzle::Identity:
                break;
            }
            if (stencil_view) {
                // DXGI's stencil SRVs expose stencil in the logical G
                // component. Metal's stencil-plane view exposes the stored
                // value through R, so preserve the DXGI component mapping.
                swizzle = MTLTextureSwizzleChannelsMake(
                    MTLTextureSwizzleZero, MTLTextureSwizzleRed,
                    MTLTextureSwizzleZero, MTLTextureSwizzleOne);
            }
            swizzle = metal_component_mapping(swizzle,
                                              requested.component_mapping);
            id<MTLTexture> view = [texture
                newTextureViewWithPixelFormat:pixel_format
                                  textureType:static_cast<MTLTextureType>(
                                                  texture_type)
                                       levels:NSMakeRange(mip_first, mip_count)
                                       slices:NSMakeRange(slice_first,
                                                          slice_count)
                                      swizzle:swizzle];
            // Binding the parent would expose subresources outside the D3D
            // SRV and produce plausible but incorrect pixels. Fail the bind
            // honestly if Metal rejects the exact view.
            if (view == nil) return nullptr;
            void* retained = (__bridge_retained void*)view;
            m_shader_views.emplace(
                std::make_pair(key, requested.component_mapping), retained);
            m_view_memo.remember(parent, requested, mipmaps_enabled, retained);
            return retained;
        }
    }

    ~MetalBackendTexture2D() override;

    MetalBackendTexture2D(const MetalBackendTexture2D&) = delete;
    MetalBackendTexture2D& operator=(const MetalBackendTexture2D&) = delete;

private:
    BackendTexture2DDesc m_desc;
    void* m_command_queue{nullptr};
    /// Shared with every other opening of a mirrored shared texture in this
    /// process (MetalSharedLocal::texture).
    std::shared_ptr<MetalTexture> m_texture;
    std::shared_ptr<MetalSharedLocal> m_shared;
    /// A planar video format's chroma plane (m_texture is the luma plane).
    std::unique_ptr<MetalBackendTexture2D> m_plane1;
    void* m_borrowed_handle{nullptr};
    /// (view key, component mapping) -> retained id<MTLTexture>.
    mutable std::map<std::pair<u64, u16>, void*> m_shader_views;
    /// The last native_shader_handle() resolution. A draw binds the same
    /// views draw after draw, and resolving one costs an autorelease pool and
    /// five Objective-C property reads of the parent: ~30k a frame at a
    /// the menu. It names the parent it was made for, so a storage
    /// swap misses it, and it is cleared with m_shader_views, whose views it
    /// may point at (a recycled storage can bring an old parent back).
    /// Threading: like m_shader_views, it is written by every resolution, so
    /// native_shader_handle() needs the callers' serialisation (the D3D11
    /// device lock, the D3D12 manager's mutex); the encode thread never
    /// calls it.
    struct ViewMemo {
        void* parent{nullptr};
        BackendTextureViewDesc requested{};
        bool mipmaps{false};
        void* result{nullptr};
        // Every field of BackendTextureViewDesc is compared below; a new
        // field must be added there, or two different views would match.
        static_assert(sizeof(BackendTextureViewDesc) == 24,
                      "BackendTextureViewDesc changed: update matches()");
        [[nodiscard]] bool matches(void* p, const BackendTextureViewDesc& r,
                                   bool m) const noexcept {
            return parent == p && mipmaps == m &&
                   requested.dimension == r.dimension &&
                   requested.swizzle == r.swizzle &&
                   requested.component_mapping == r.component_mapping &&
                   requested.pixel_format == r.pixel_format &&
                   requested.most_detailed_mip == r.most_detailed_mip &&
                   requested.mip_levels == r.mip_levels &&
                   requested.first_array_slice == r.first_array_slice &&
                   requested.array_size == r.array_size;
        }
        void remember(void* p, const BackendTextureViewDesc& r, bool m,
                      void* view) noexcept {
            parent = p;
            requested = r;
            mipmaps = m;
            result = view;
        }
    };
    mutable ViewMemo m_view_memo;
    /// What discard_storage() needs to allocate a twin.
    void* m_device{nullptr};
    TextureDesc m_metal_desc{};
    /// Storage replaced by a DISCARD, with the recording generation that last
    /// could have used it; reused once the GPU has retired that generation.
    std::vector<std::pair<std::shared_ptr<MetalTexture>, u64>> m_discarded;
    /// Set by discard_storage() for the upload that follows it: storage just
    /// renamed in is referenced by no recorded work (a reused twin only once
    /// its retire generation has completed), so the CPU writes it at once.
    bool m_storage_fresh{false};
};

} // namespace vitreo::graphics
