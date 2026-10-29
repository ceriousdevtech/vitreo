// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_backend_texture_2d_clear.mm
/// @brief MetalBackendTexture2D clears and fills

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

#include "graphics/metal/metal_backend_texture_2d.hpp"
#include "graphics/metal/metal_backend_texture_ops.hpp"

using namespace vitreo;

namespace vitreo::graphics {

bool MetalBackendTexture2D::clear_render_target(
    const std::array<float, 4>& color) {
    if (m_plane1 && !m_plane1->clear_render_target(color)) return false;
    if (!valid() || !m_desc.render_target) return false;
    // Clear every mip — sampling an uninitialised higher level reads
    // as neon green on Apple GPUs (a 64² render-target mip chain).
    for (unsigned level = 0; level < m_desc.mip_levels; ++level) {
        if (!clear_render_target_level(color, level)) return false;
    }
    return true;
}

bool MetalBackendTexture2D::clear_render_target_level(
    const std::array<float, 4>& color, unsigned mip_level,
    unsigned first_array_slice, unsigned array_size) {
    return clear_render_target_through(native_handle(), color, mip_level,
                                       first_array_slice, array_size);
}

bool MetalBackendTexture2D::clear_render_target_view(
    const std::array<float, 4>& color, PixelFormat view_format,
    unsigned mip_level, unsigned first_array_slice, unsigned array_size) {
    if (chroma_view(view_format)) {
        return m_plane1->clear_render_target_view(
            color, view_format, mip_level, first_array_slice, array_size);
    }
    // A view of the RTV's format, so Metal converts the colour as that
    // type: sRGB-encoded, integral, signed-normalized.
    return clear_render_target_through(
        native_render_target_handle(view_format), color, mip_level,
        first_array_slice, array_size);
}

bool MetalBackendTexture2D::clear_render_target_through(
    void* attachment, const std::array<float, 4>& color, unsigned mip_level,
    unsigned first_array_slice, unsigned array_size) {
    if (!valid() || attachment == nullptr || !m_desc.render_target ||
        mip_level >= m_desc.mip_levels || array_size == 0 ||
        first_array_slice >= m_desc.array_size ||
        array_size > m_desc.array_size - first_array_slice) {
        return false;
    }
    CsBeginPass begin{};
    begin.colors[0] = attachment;
    begin.color_count = 1;
    begin.clear_color = true;
    begin.color_clear = color;
    begin.color_levels[0] = mip_level;
    begin.color_slices[0] = first_array_slice;
    begin.render_target_array_length = array_size;
    const u64 pass_id = MetalSubmitQueue::instance().record_begin_pass(begin);
    MetalSubmitQueue::instance().record_end_pass(pass_id);
    return true;
}

bool MetalBackendTexture2D::clear_depth_stencil(float depth, u8 stencil) {
    return clear_depth_stencil_level(depth, stencil, 0, 0, 1);
}

bool MetalBackendTexture2D::clear_depth_stencil_level(
    float depth, u8 stencil, unsigned mip_level, unsigned first_array_slice,
    unsigned array_size) {
    return clear_depth_stencil_aspects(
        depth, stencil, true, true, mip_level,
        first_array_slice, array_size);
}

bool MetalBackendTexture2D::clear_depth_stencil_aspects(
    float depth, u8 stencil, bool clear_depth, bool clear_stencil,
    unsigned mip_level, unsigned first_array_slice, unsigned array_size) {
    if (!valid() || !m_desc.depth_stencil ||
        (!clear_depth && !clear_stencil) ||
        mip_level >= m_desc.mip_levels || array_size == 0 ||
        first_array_slice >= m_desc.array_size ||
        array_size > m_desc.array_size - first_array_slice) {
        return false;
    }
    CsBeginPass begin{};
    begin.depth = m_texture->handle();
    begin.depth_level = mip_level;
    begin.depth_slice = first_array_slice;
    begin.render_target_array_length = array_size;
    begin.clear_depth = clear_depth;
    begin.clear_stencil = false;
    begin.depth_clear = depth;
    const FormatInfo format = format_table(m_desc.pixel_format);
    if (format.mtl_pixel_format == 255 || // Depth24Unorm_Stencil8
        format.mtl_pixel_format == 260) { // Depth32Float_Stencil8
        begin.stencil = true;
        begin.clear_stencil = clear_stencil;
        begin.stencil_clear = stencil;
    } else if (clear_stencil && !clear_depth) {
        // Depth32Float has no stencil plane. A stencil-only clear cannot
        // be recorded. A combined clear still writes depth: retail D3D11
        // applies the aspects the view has.
        return false;
    }
    const u64 pass_id = MetalSubmitQueue::instance().record_begin_pass(begin);
    MetalSubmitQueue::instance().record_end_pass(pass_id);
    return true;
}

bool MetalBackendTexture2D::fill_depth_stencil(
    bool stencil_plane, float depth, u8 stencil, unsigned mip_level,
    unsigned first_array_slice, unsigned array_size, unsigned x, unsigned y,
    unsigned width, unsigned height) {
    const FormatInfo format = format_table(m_desc.pixel_format);
    if (!valid() || !format.is_depth || m_desc.sample_count > 1 ||
        mip_level >= m_desc.mip_levels || array_size == 0 ||
        first_array_slice >= m_desc.array_size ||
        array_size > m_desc.array_size - first_array_slice ||
        width == 0 || height == 0) {
        return false;
    }
    const unsigned level_width = std::max(1u, m_desc.width >> mip_level);
    const unsigned level_height = std::max(1u, m_desc.height >> mip_level);
    if (x >= level_width || y >= level_height ||
        width > level_width - x || height > level_height - y) {
        return false;
    }
    // A blit into a depth texture writes the stored value; a combined
    // format's blit names the plane (Depth32Float_Stencil8 keeps them
    // apart: a float per texel, a byte per texel).
    const float d = std::clamp(depth, 0.0f, 1.0f);
    std::array<u8, 4> texel{};
    unsigned texel_bytes = 0;
    u32 options = MTLBlitOptionNone;
    switch (format.mtl_pixel_format) {
    case MTLPixelFormatDepth32Float:
        if (stencil_plane) return false;
        std::memcpy(texel.data(), &d, 4);
        texel_bytes = 4;
        break;
    case MTLPixelFormatDepth16Unorm: {
        if (stencil_plane) return false;
        const auto unorm =
            static_cast<u16>(std::nearbyint(d * 65535.0f));
        std::memcpy(texel.data(), &unorm, 2);
        texel_bytes = 2;
        break;
    }
    case MTLPixelFormatDepth32Float_Stencil8:
        if (stencil_plane) {
            texel[0] = stencil;
            texel_bytes = 1;
            options = MTLBlitOptionStencilFromDepthStencil;
        } else {
            std::memcpy(texel.data(), &d, 4);
            texel_bytes = 4;
            options = MTLBlitOptionDepthFromDepthStencil;
        }
        break;
    default:
        return false;
    }
    m_storage_fresh = false;
    return record_fill_blit(native_handle(), texel.data(), texel_bytes,
                            mip_level, first_array_slice, array_size, 0,
                            x, y, width, height, 1, options);
}

bool MetalBackendTexture2D::fill_texels(
    const void* texel, unsigned texel_bytes, unsigned mip_level,
    unsigned first_array_slice, unsigned array_size, unsigned x, unsigned y,
    unsigned width, unsigned height) {
    const FormatInfo format = format_table(m_desc.pixel_format);
    if (!valid() || texel == nullptr || format.is_compressed ||
        format.is_depth || m_desc.sample_count > 1 ||
        texel_bytes == 0 || texel_bytes != format.bytes_per_pixel ||
        mip_level >= m_desc.mip_levels || array_size == 0 ||
        first_array_slice >= m_desc.array_size ||
        array_size > m_desc.array_size - first_array_slice ||
        width == 0 || height == 0) {
        return false;
    }
    const unsigned level_width = std::max(1u, m_desc.width >> mip_level);
    const unsigned level_height = std::max(1u, m_desc.height >> mip_level);
    if (x >= level_width || y >= level_height ||
        width > level_width - x || height > level_height - y) {
        return false;
    }
    m_storage_fresh = false;
    if (m_desc.unordered_access) {
        // An unsigned-integer view of the same texel size stores the
        // bits verbatim, whatever the resource's own type.
        BackendTextureViewDesc view{};
        view.pixel_format = uint_view_format(texel_bytes);
        view.dimension = BackendTextureViewDimension::Texture2DArray;
        view.most_detailed_mip = mip_level;
        view.mip_levels = 1;
        view.first_array_slice = 0;
        view.array_size = m_desc.array_size;
        void* handle = view.pixel_format == PixelFormat::Unknown
                           ? nullptr
                           : native_shader_handle(view, true);
        if (handle != nullptr &&
            record_fill_dispatch(false, handle, texel, texel_bytes, x, y,
                                 first_array_slice, width, height,
                                 array_size)) {
            return true;
        }
    }
    // Any other texture: blit a buffer of the texel into each slice.
    return record_fill_blit(native_handle(), texel, texel_bytes,
                            mip_level, first_array_slice, array_size, 0,
                            x, y, width, height);
}

bool MetalBackendTexture2D::fill_depth_stencil_texels(
    u32 depth_bits, bool write_stencil, u8 stencil, unsigned mip_level,
    unsigned first_array_slice, unsigned array_size, unsigned x, unsigned y,
    unsigned width, unsigned height) {
    // A buffer-to-texture blit stores the bits as given, which a
    // render-pass clear (clearDepth) cannot promise for values outside
    // [0, 1]; a combined depth+stencil texture takes each plane through
    // its own blit option. Multisampled textures take no blits.
    if (!valid() || !m_desc.depth_stencil || m_desc.sample_count > 1 ||
        mip_level >= m_desc.mip_levels || array_size == 0 ||
        first_array_slice >= m_desc.array_size ||
        array_size > m_desc.array_size - first_array_slice ||
        width == 0 || height == 0) {
        return false;
    }
    const unsigned level_width = std::max(1u, m_desc.width >> mip_level);
    const unsigned level_height = std::max(1u, m_desc.height >> mip_level);
    if (x >= level_width || y >= level_height ||
        width > level_width - x || height > level_height - y) {
        return false;
    }
    const auto mtl_format = static_cast<MTLPixelFormat>(
        format_table(m_desc.pixel_format).mtl_pixel_format);
    void* handle = native_handle();
    if (handle == nullptr) return false;
    m_storage_fresh = false;
    switch (mtl_format) {
    case MTLPixelFormatDepth16Unorm: {
        const u16 code = static_cast<u16>(depth_bits);
        return record_fill_blit(handle, &code, sizeof(code), mip_level,
                                first_array_slice, array_size, 0, x, y,
                                width, height);
    }
    case MTLPixelFormatDepth32Float:
        return record_fill_blit(handle, &depth_bits, sizeof(depth_bits),
                                mip_level, first_array_slice, array_size,
                                0, x, y, width, height);
    case MTLPixelFormatDepth32Float_Stencil8:
        return record_fill_blit(
                   handle, &depth_bits, sizeof(depth_bits), mip_level,
                   first_array_slice, array_size, 0, x, y, width, height,
                   1, static_cast<u32>(MTLBlitOptionDepthFromDepthStencil)) &&
               (!write_stencil ||
                record_fill_blit(
                    handle, &stencil, sizeof(stencil), mip_level,
                    first_array_slice, array_size, 0, x, y, width, height,
                    1,
                    static_cast<u32>(MTLBlitOptionStencilFromDepthStencil)));
    default:
        return false;
    }
}

} // namespace vitreo::graphics
