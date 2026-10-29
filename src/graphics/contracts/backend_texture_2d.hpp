// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_texture_2d.hpp
/// @brief 2D, array, cube and multisample textures and their asynchronous readback.

#include "graphics/contracts/backend_memory.hpp"
#include "graphics/contracts/format_converter.hpp"
#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace vitreo::graphics {

struct BackendTexture2DDesc {
    unsigned width{0};
    unsigned height{0};
    unsigned mip_levels{1};
    unsigned array_size{1};
    PixelFormat pixel_format{PixelFormat::Unknown};
    /// Resource was created from a typeless guest format and may be viewed as
    /// a compatible typed member of its pixel-format family.
    bool pixel_format_view{false};
    /// When true, create a cube map (six square faces). `width` must equal
    /// `height` (the edge length). `array_size` is the face count: 6 for one
    /// cube, a multiple of 6 for a cube array. `array_size == 1` is accepted
    /// as a single cube so D3D9 and D3D11 can share this descriptor.
    bool cube{false};
    bool render_target{false};
    bool shader_resource{false};
    /// Request CPU-visible (Shared) storage so the backend can `replaceRegion`.
    /// D3D9 `D3DUSAGE_DYNAMIC` needs this. Ordinary LockRect/UnlockRect uploads
    /// should leave this false so the backend can keep Private GPU storage and
    /// blit from a staging buffer.
    bool cpu_writable{false};
    /// Unordered-access view: GPU can write to this texture from compute or
    /// pixel shaders. D3D11 UAV textures require explicit sync before read.
    bool unordered_access{false};
    /// Depth-stencil target attachment. Depth formats must set this flag.
    /// A texture can be both depth_stencil and shader_resource when the
    /// frontend supports depth reads.
    bool depth_stencil{false};
    /// The texture may be opened by another backend device or process by the
    /// opaque handle BackendTexture2D::export_shared() returns. A single-
    /// sample, single-mip, single-slice 2D texture of a format
    /// GraphicsBackendDevice::supports_shared_texture_2d accepts lives in
    /// that shared storage directly (zero copy); any other shape of a format
    /// supports_shared_texture accepts keeps its own storage and is mirrored
    /// into a shared canonical store at the synchronization points
    /// (GraphicsBackendDevice::publish_shared_writes / acquire_shared_writes).
    bool shareable{false};
    /// Opaque frontend bytes stored WITH a shareable texture's storage, so
    /// every opener (another device, another process) reads back exactly
    /// what the creator described -- BackendTexture2D::shared_metadata().
    /// Ignored when the texture is not shared.
    std::vector<u8> shared_metadata;
    /// Backend-internal: the local texture of another process's mirrored
    /// shared texture (created from its store's record). It needs the
    /// usages its pulls and pushes do, without a store of its own.
    bool shared_mirror{false};
    /// D3D9 luminance formats store a single channel but sample as (L,L,L,1)
    /// or (L,L,L,A). Metal R8/RG8 return (R,0,0,1)/(R,G,0,1) unless swizzled.
    enum class ChannelSwizzle : u8 {
        Identity = 0,
        Luminance,      // L8: RGB←R, A←1
        LuminanceAlpha, // A8L8: RGB←R(L), A←G(A)
    };
    ChannelSwizzle channel_swizzle{ChannelSwizzle::Identity};
    /// `1` is a single-sample texture. Values > 1 create a 2D multisample
    /// render target (D3DMULTISAMPLE_n_SAMPLES). MSAA textures are not
    /// shader-sampled; Present / StretchRect resolve them.
    unsigned sample_count{1};
    /// A tiled texture (D3D11_RESOURCE_MISC_TILED): address space only, laid
    /// out in 64 KiB tiles (tiled_texture_layout) that update_tile_mappings
    /// points at tile-pool memory. Every tile starts unmapped.
    bool tiled{false};
    /// Place the texture's storage inside @p heap at @p heap_offset instead
    /// of allocating its own (D3D12 CreatePlacedResource): it then aliases
    /// every other resource placed over the same bytes. The offset must be
    /// a multiple of the alignment heap_texture_placement reports, and the
    /// heap must outlive the texture. Null: the texture owns its storage.
    const BackendHeap* heap{nullptr};
    u64 heap_offset{0};
};

/// A GPU -> CPU texture copy recorded on the command timeline. D3D11
/// CopyResource into a STAGING texture is "an asynchronous call" (Learn) and
/// Map(DO_NOT_WAIT) of the staging texture answers WAS_STILL_DRAWING until
/// the copy has executed, so the copy must not be a blocking readback.
class BackendReadback {
public:
    virtual ~BackendReadback() = default;
    /// True once the GPU has executed the copy. Never blocks; hands the
    /// recording that holds the copy to the GPU so a poll makes progress.
    [[nodiscard]] virtual bool ready() = 0;
    /// Block until the copy has executed. False if it never will.
    [[nodiscard]] virtual bool wait() = 0;
    /// After ready()/wait(): the copied level, rows `row_pitch` bytes apart
    /// (a compressed level's rows are rows of 4x4 blocks).
    [[nodiscard]] virtual bool read(void* data, unsigned row_pitch) = 0;
};

class BackendTexture2D {
public:
    virtual ~BackendTexture2D() = default;

    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual const BackendTexture2DDesc& desc() const noexcept = 0;
    /// The global id of the texture's shared storage, once it has one (a
    /// zero-copy shared texture has one from creation, a mirrored one from
    /// its first export_shared()); 0 otherwise.
    [[nodiscard]] virtual u64 shared_handle() const noexcept { return 0; }
    /// The plane a view of @p view_format addresses: this texture, or for a
    /// planar video format (PixelFormat::NV12 / P010) and a chroma view
    /// format (planar_chroma_format and its integer twin) the chroma plane,
    /// a texture of half the width and height.
    [[nodiscard]] virtual BackendTexture2D* view_plane(
        PixelFormat view_format) noexcept {
        (void)view_format;
        return this;
    }
    /// The id another device or process opens this shareable texture by
    /// (GraphicsBackendDevice::open_shared_resource). A mirrored texture gets
    /// its canonical store here, with the current contents published to it.
    /// 0 when the texture was not created shareable.
    [[nodiscard]] virtual u64 export_shared() { return shared_handle(); }
    /// The creator's BackendTexture2DDesc::shared_metadata, as stored with the
    /// shared storage (empty for an unshared texture, or one whose creator
    /// stored none).
    [[nodiscard]] virtual const std::vector<u8>& shared_metadata()
        const noexcept {
        static const std::vector<u8> none;
        return none;
    }
    [[nodiscard]] virtual bool clear_render_target(
        const std::array<float, 4>& color) = 0;
    /// Clear a render-target mip and 2D-array slice window. Single-level,
    /// non-array backends inherit the level-0/slice-0 behavior.
    [[nodiscard]] virtual bool clear_render_target_level(
        const std::array<float, 4>& color, unsigned mip_level,
        unsigned first_array_slice = 0, unsigned array_size = 1) {
        return mip_level == 0 && first_array_slice == 0 && array_size == 1 &&
               clear_render_target(color);
    }

    /// Clear depth/stencil attachment. Returns false if texture is not a
    /// depth_stencil target. `stencil` is ignored for depth-only formats.
    [[nodiscard]] virtual bool clear_depth_stencil(float depth,
                                                    u8 stencil) = 0;

    [[nodiscard]] virtual bool clear_depth_stencil_level(
        float depth, u8 stencil, unsigned mip_level,
        unsigned first_array_slice = 0, unsigned array_size = 1) {
        return mip_level == 0 && first_array_slice == 0 && array_size == 1 &&
               clear_depth_stencil(depth, stencil);
    }
    /// Clear selected depth/stencil aspects while preserving any aspect not
    /// selected. Legacy backends inherit combined-clear support only.
    [[nodiscard]] virtual bool clear_depth_stencil_aspects(
        float depth, u8 stencil, bool clear_depth, bool clear_stencil,
        unsigned mip_level, unsigned first_array_slice = 0,
        unsigned array_size = 1) {
        return clear_depth && clear_stencil &&
               clear_depth_stencil_level(depth, stencil, mip_level,
                                         first_array_slice, array_size);
    }

    /// Clear a render-target window through a typed view of the resource. A
    /// D3D render-target view names its own format and the clear colour is
    /// "converted to the fully qualified format type of the View" (D3D11.3
    /// 5.2.3.1), which differs from the resource's for a typeless resource or
    /// an sRGB view of a UNORM back buffer. Unknown, or the resource's own
    /// format, is clear_render_target_level; a backend without typed views
    /// fails any other format rather than clearing as the wrong type.
    [[nodiscard]] virtual bool clear_render_target_view(
        const std::array<float, 4>& color, PixelFormat view_format,
        unsigned mip_level, unsigned first_array_slice,
        unsigned array_size) {
        if (view_format != PixelFormat::Unknown &&
            view_format != desc().pixel_format) {
            return false;
        }
        return clear_render_target_level(color, mip_level, first_array_slice,
                                         array_size);
    }

    /// Store one texel into every texel of a rectangle of one mip, across
    /// `array_size` array slices from `first_array_slice`, in order with the
    /// work already recorded. `texel` is `texel_bytes` bytes already in the
    /// storage format's byte layout, and `texel_bytes` must equal that
    /// format's bytes per pixel. This is how a clear stores bits a render-pass
    /// clear colour cannot express, or into a texture that is not a render
    /// target: D3D ClearUnorderedAccessViewUint/Float and ClearView rectangles.
    /// Default: unsupported, so the caller fails the clear visibly.
    /// Store @p depth (the depth plane, converted to the storage format the
    /// way a depth clear is) or @p stencil (the stencil plane) into a
    /// rectangle of one mip of a depth / depth+stencil texture, across
    /// `array_size` slices, in order with the work already recorded: D3D12
    /// ClearDepthStencilView rectangles. Default: unsupported.
    [[nodiscard]] virtual bool fill_depth_stencil(bool stencil_plane,
                                                  float depth, u8 stencil,
                                                  unsigned mip_level,
                                                  unsigned first_array_slice,
                                                  unsigned array_size,
                                                  unsigned x, unsigned y,
                                                  unsigned width,
                                                  unsigned height) {
        (void)stencil_plane;
        (void)depth;
        (void)stencil;
        (void)mip_level;
        (void)first_array_slice;
        (void)array_size;
        (void)x;
        (void)y;
        (void)width;
        (void)height;
        return false;
    }

    [[nodiscard]] virtual bool fill_texels(const void* texel,
                                           unsigned texel_bytes,
                                           unsigned mip_level,
                                           unsigned first_array_slice,
                                           unsigned array_size, unsigned x,
                                           unsigned y, unsigned width,
                                           unsigned height) {
        (void)texel;
        (void)texel_bytes;
        (void)mip_level;
        (void)first_array_slice;
        (void)array_size;
        (void)x;
        (void)y;
        (void)width;
        (void)height;
        return false;
    }

    /// Store one depth value -- and, when @p write_stencil, one stencil value
    /// -- into a rectangle of one mip of a depth texture, across `array_size`
    /// array slices from `first_array_slice`, in order with the work already
    /// recorded and bit-exact: @p depth_bits is the depth plane's storage
    /// texel (a 16-bit code for Depth16Unorm, the float's bits for
    /// Depth32Float and the depth plane of Depth32Float_Stencil8). This is
    /// D3D ClearView on a depth-stencil view, whose values need not be ones a
    /// render-pass clear can store (out-of-range and non-finite floats).
    /// False for a multisampled texture, a format without such planes, or a
    /// rectangle outside the level. Default: unsupported.
    [[nodiscard]] virtual bool fill_depth_stencil_texels(
        u32 depth_bits, bool write_stencil, u8 stencil, unsigned mip_level,
        unsigned first_array_slice, unsigned array_size, unsigned x,
        unsigned y, unsigned width, unsigned height) {
        (void)depth_bits;
        (void)write_stencil;
        (void)stencil;
        (void)mip_level;
        (void)first_array_slice;
        (void)array_size;
        (void)x;
        (void)y;
        (void)width;
        (void)height;
        return false;
    }

    /// D3D11 Map(WRITE_DISCARD) of a CPU-writable texture: move the texture to
    /// new storage, so the draws and dispatches already recorded against it
    /// keep reading the texels they were recorded with (Learn,
    /// D3D11_MAP_WRITE_DISCARD: "the previous contents of the resource will be
    /// undefined" -- to the CPU, never to work already queued). The new
    /// storage's contents are undefined. False when the backend cannot rename;
    /// the caller must then order the upload itself.
    [[nodiscard]] virtual bool discard_storage() { return false; }

    /// Replace the pixels of one mip level from CPU memory.
    ///
    /// `row_pitch` is the source stride in bytes; 0 means tightly packed.
    /// Shared (`cpu_writable`) textures may use a CPU `replaceRegion`; Private
    /// textures upload through a GPU blit. Returns false when the level is out
    /// of range or the backend rejected the upload.
    [[nodiscard]] virtual bool update(unsigned mip_level, const void* data,
                                      unsigned row_pitch) = 0;

    /// Replace one mip level of one array/cube slice from CPU memory.
    /// Slice 0 is the only valid slice for ordinary 2D textures; cube maps
    /// use slices 0..5 for the six faces (D3DCUBEMAP_FACES order).
    [[nodiscard]] virtual bool update_slice(unsigned mip_level, unsigned slice,
                                            const void* data,
                                            unsigned row_pitch) {
        if (slice != 0) return false;
        return update(mip_level, data, row_pitch);
    }

    /// Replace a texel rectangle in one mip level and array slice.
    /// `data` begins at the rectangle's top-left texel; `row_pitch` is the
    /// source row stride. Backends may inherit full-level updates only.
    [[nodiscard]] virtual bool update_region(
        unsigned mip_level, unsigned slice, const void* data,
        unsigned row_pitch, unsigned x, unsigned y,
        unsigned width, unsigned height) {
        const unsigned level_width = std::max(1u, desc().width >> mip_level);
        const unsigned level_height = std::max(1u, desc().height >> mip_level);
        return x == 0 && y == 0 && width == level_width &&
               height == level_height &&
               update_slice(mip_level, slice, data, row_pitch);
    }

    /// Copy one mip level into caller-provided native-format CPU memory.
    ///
    /// `row_pitch` is the destination stride in bytes. Backends return false
    /// when the format or mip cannot be read without changing its native byte
    /// representation. D3D11 uses this for DEFAULT-to-STAGING transfers before
    /// exposing the staging resource through Map.
    [[nodiscard]] virtual bool readback(unsigned mip_level, void* data,
                                        unsigned row_pitch) {
        (void)mip_level;
        (void)data;
        (void)row_pitch;
        return false;
    }

    /// Copy one mip level of one array/cube slice into native-format memory.
    [[nodiscard]] virtual bool readback_slice(unsigned mip_level,
                                              unsigned slice, void* data,
                                              unsigned row_pitch) {
        if (slice != 0) return false;
        return readback(mip_level, data, row_pitch);
    }

    /// Record a copy of one mip level of one array slice into host-visible
    /// memory, in order with the recorded work, and return without waiting
    /// for it. Null when the backend cannot (the caller then reads back
    /// synchronously through readback_slice).
    [[nodiscard]] virtual std::shared_ptr<BackendReadback> readback_slice_async(
        unsigned /*mip_level*/, unsigned /*slice*/) {
        return nullptr;
    }

    /// Read one plane of a combined depth+stencil texture: the depth plane as
    /// one 32-bit float per texel, or (@p stencil) the stencil plane as one
    /// byte per texel. `readback_slice` cannot read such a texture -- the host
    /// keeps the planes apart -- so a D3D GPU-to-staging copy of a packed
    /// depth+stencil format reads both through this and repacks them.
    [[nodiscard]] virtual bool readback_depth_stencil_plane(
        unsigned /*mip_level*/, unsigned /*slice*/, bool /*stencil*/,
        void* /*data*/, unsigned /*row_pitch*/) {
        return false;
    }
    /// The inverse of readback_depth_stencil_plane: replace one plane of a
    /// combined depth+stencil subresource from `data` (one 32-bit float per
    /// texel for depth, one byte for stencil), in order with the recorded
    /// work. A D3D upload of a packed depth+stencil format (staging D24S8 ->
    /// DEFAULT) arrives split into these. Default: unsupported.
    [[nodiscard]] virtual bool update_depth_stencil_plane(
        unsigned /*mip_level*/, unsigned /*slice*/, bool /*stencil*/,
        const void* /*data*/, unsigned /*row_pitch*/) {
        return false;
    }

    /// Replace a contiguous row range `[y, y+height)` of one mip.
    ///
    /// Default falls back to a full-level `update` when `y == 0` and
    /// `height` covers the level; otherwise returns false. Metal implements
    /// true subrect uploads so large slideshow stills can pump AppKit
    /// between strips.
    [[nodiscard]] virtual bool update_rows(unsigned mip_level,
                                           const void* data,
                                           unsigned row_pitch,
                                           unsigned y,
                                           unsigned height) {
        (void)mip_level;
        (void)data;
        (void)row_pitch;
        (void)y;
        (void)height;
        return false;
    }
};

} // namespace vitreo::graphics
