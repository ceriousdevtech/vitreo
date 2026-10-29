// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_texture_3d.hpp
/// @brief Volume textures and texture copy locations.

#include "graphics/contracts/backend_texture_2d.hpp"
#include <array>
#include <vector>

namespace vitreo::graphics {

struct BackendTexture3DDesc {
    unsigned width{0};
    unsigned height{0};
    unsigned depth{0};
    unsigned mip_levels{1};
    PixelFormat pixel_format{PixelFormat::Unknown};
    bool shader_resource{false};
    /// A volume texture the guest renders into. D3D expresses this as
    /// D3D11_BIND_RENDER_TARGET on a Texture3D, whose render-target view
    /// selects a mip and a range of W slices.
    bool render_target{false};
    /// A volume a compute shader writes through a UAV (RWTexture3D<T>):
    /// D3D11_BIND_UNORDERED_ACCESS on a Texture3D.
    bool unordered_access{false};
    /// Request CPU-visible storage. Ordinary LockBox uploads should leave this
    /// false so the backend can blit into Private GPU storage.
    bool cpu_writable{false};
    /// As BackendTexture2DDesc::pixel_format_view: shader views may read the
    /// volume under another pixel format of the same texel size (a D3D
    /// TYPELESS volume's typed SRVs). A UAV-bound volume allows it anyway.
    bool pixel_format_view{false};
    /// As BackendTexture2DDesc::shareable (always mirrored through a store).
    bool shareable{false};
    std::vector<u8> shared_metadata;
    /// As BackendTexture2DDesc::heap / heap_offset.
    const BackendHeap* heap{nullptr};
    u64 heap_offset{0};
};

class BackendTexture3D {
public:
    virtual ~BackendTexture3D() = default;

    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual const BackendTexture3DDesc& desc() const noexcept = 0;
    /// What a shader binds for a view of this volume: mips [first_mip,
    /// first_mip + mip_count) read or written as @p format (Unknown = the
    /// volume's own), which must have the storage's texel size -- a D3D
    /// typed view of a TYPELESS volume, or an R32 UAV cast (functional spec
    /// 5.3.9.5). This volume itself when that is all of it as stored;
    /// otherwise a view the volume owns and keeps, built on first use. Null
    /// when the backend cannot express it. A view always spans every W
    /// slice of its mips.
    [[nodiscard]] virtual BackendTexture3D* view(PixelFormat format,
                                                 unsigned first_mip,
                                                 unsigned mip_count) {
        const auto& own = desc();
        if ((format == PixelFormat::Unknown || format == own.pixel_format) &&
            first_mip == 0 && mip_count == own.mip_levels) {
            return this;
        }
        return nullptr;
    }
    /// As BackendTexture2D::shared_handle / export_shared.
    [[nodiscard]] virtual u64 shared_handle() const noexcept { return 0; }
    [[nodiscard]] virtual u64 export_shared() { return shared_handle(); }
    /// Copy one W slice of one mip into host memory (native format), after
    /// all previously recorded GPU work. Diagnostics and D3D readback.
    [[nodiscard]] virtual bool readback_w_slice(unsigned /*mip_level*/,
                                                unsigned /*w_slice*/,
                                                void* /*data*/,
                                                unsigned /*row_pitch*/) {
        return false;
    }

    /// Replace one mip level from tightly stacked CPU slices.
    ///
    /// `row_pitch` is the source stride in bytes (0 = tightly packed rows).
    /// `slice_pitch` is the source stride between depth slices (0 = tightly
    /// packed slices of `row_pitch * level_height` bytes).
    [[nodiscard]] virtual bool update(unsigned mip_level, const void* data,
                                      unsigned row_pitch,
                                      unsigned slice_pitch) = 0;

    /// Replace a box of one mip from CPU memory: `data` starts at the box's
    /// first texel (or 4x4 block), rows `row_pitch` and W slices
    /// `slice_pitch` bytes apart. A block-compressed box is in texels, block
    /// aligned or reaching the level's edge. Ordered with the recorded work.
    /// Default: unsupported, so the caller drops the update visibly.
    [[nodiscard]] virtual bool update_region(
        unsigned mip_level, const void* data, unsigned row_pitch,
        unsigned slice_pitch, unsigned x, unsigned y, unsigned z,
        unsigned width, unsigned height, unsigned depth) {
        (void)mip_level;
        (void)data;
        (void)row_pitch;
        (void)slice_pitch;
        (void)x;
        (void)y;
        (void)z;
        (void)width;
        (void)height;
        (void)depth;
        return false;
    }

    /// The volume form of BackendTexture2D::fill_texels: one texel into a
    /// box of one mip.
    [[nodiscard]] virtual bool fill_texels(const void* texel,
                                           unsigned texel_bytes,
                                           unsigned mip_level, unsigned x,
                                           unsigned y, unsigned z,
                                           unsigned width, unsigned height,
                                           unsigned depth) {
        (void)texel;
        (void)texel_bytes;
        (void)mip_level;
        (void)x;
        (void)y;
        (void)z;
        (void)width;
        (void)height;
        (void)depth;
        return false;
    }

    /// Clear a window of W slices of one mip. This is the volume analogue of
    /// BackendTexture2D::clear_render_target_level, and is what a D3D
    /// ClearRenderTargetView on a Texture3D view lowers to. Default is
    /// unsupported so a backend without volume render targets fails the clear
    /// visibly rather than silently dropping it.
    [[nodiscard]] virtual bool clear_render_target_slices(
        const std::array<float, 4>& color, unsigned mip_level,
        unsigned first_w_slice, unsigned w_size) {
        (void)color;
        (void)mip_level;
        (void)first_w_slice;
        (void)w_size;
        return false;
    }
};

/// One corner of a texture copy: a 2D (array/cube) or a volume texture, the
/// mip and array slice, and the origin in that level's texels.
struct BackendTextureLocation {
    BackendTexture2D* texture_2d{nullptr};
    BackendTexture3D* texture_3d{nullptr};
    unsigned mip{0};
    unsigned slice{0};
    unsigned x{0};
    unsigned y{0};
    unsigned z{0};
};

} // namespace vitreo::graphics
