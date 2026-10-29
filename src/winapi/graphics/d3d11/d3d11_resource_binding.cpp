// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_resource_binding.cpp
/// @brief Shader-resource, UAV, constant-buffer and indirect-argument
///        resolution for draws and dispatches.
///        Split out of d3d11_pipeline.cpp as verbatim code motion.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_pipeline_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "winapi/graphics/d3d11/d3d11_draw_memo.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/graphics/common/live_diag.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "winapi/graphics/common/d3d_sampler.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <vitreo/diagnostics.hpp>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <map>
#include <tuple>
#include <mutex>
#include <vector>
#include <cctype>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <mach/mach_time.h>

namespace vitreo::winapi {

namespace {

// Learn D3D11_UAV_DIMENSION: TEXTURE3D.
constexpr u32 kD3D11UavDimensionTexture3D = 8;
// Learn D3D11_RESOURCE_MISC_FLAG: an indirect-argument buffer must carry this.
// Spelled locally rather than pulled from d3d11_internal.hpp, whose IID block
// collides with this file's own.
constexpr u32 D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS = 0x10u;

} // namespace

const graphics::BackendBuffer* D3D11Manager::resolve_indirect_args(
    GuestAddr buffer_interface, u32 offset, const char* api,
    u32 block_bytes) {
    if (buffer_interface == 0) {
        pipeline_diag("%s: null argument buffer", api);
        return nullptr;
    }
    const auto buffer = m_buffers.find(buffer_interface);
    if (buffer == m_buffers.end() || !buffer->second.backend) {
        pipeline_diag("%s: argument buffer 0x%llx is not a live buffer", api,
                      static_cast<unsigned long long>(buffer_interface));
        return nullptr;
    }
    if ((buffer->second.desc.misc_flags &
         D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) == 0) {
        pipeline_diag(
            "%s: argument buffer 0x%llx lacks "
            "D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS (misc=0x%x)",
            api, static_cast<unsigned long long>(buffer_interface),
            buffer->second.desc.misc_flags);
        return nullptr;
    }
    // Learn: AlignedByteOffsetForArgs is a DWORD-aligned byte offset (the
    // D3D11.3 spec, 8.7 and 8.8: "A DWORD aligned byte offset";
    // DispatchIndirect's page says "byte-aligned", which WARP does not honour,
    // below). The block the call fetches -- 4 UINTs for DrawInstancedIndirect,
    // 5 for DrawIndexedInstancedIndirect, 3 for DispatchIndirect -- has to lie
    // inside the buffer: the spec calls a fetch past its end "undefined", and
    // the runtime's answer, measured on WARP
    // (tools/oracle_probe/d3d11_indirect_args_bounds_probe.expected), is to
    // drop the call -- no vertex, primitive or thread runs -- when the block
    // overruns the buffer by even its last dword, when the offset is at or
    // past the end, and for every offset that is not a multiple of 4, for all
    // three calls alike. Dropping is also the only safe answer here: the
    // bytes past the buffer are not the application's.
    if ((offset % 4) != 0) {
        pipeline_diag("%s: argument offset %u is not DWORD aligned (call "
                      "dropped)", api, offset);
        return nullptr;
    }
    if (static_cast<u64>(offset) + block_bytes >
        buffer->second.desc.byte_width) {
        pipeline_diag("%s: the %u-byte argument block at offset %u does not "
                      "fit a %u-byte buffer (call dropped)", api, block_bytes,
                      offset, buffer->second.desc.byte_width);
        return nullptr;
    }
    return buffer->second.backend.get();
}

// =============================================================================
// Shader-resource resolution
// =============================================================================
//
// These were lambdas inside execute_draw. They are members now because three
// callers need the same answer: the flat draw path, the flat dispatch path, and
// the descriptor-table sync. A second copy of the SRV view-dimension and
// D3D11_FILTER decoding is exactly the kind of duplication that drifts silently
// -- both copies keep rendering, just differently.

namespace {
[[nodiscard]] usize lookup_memo_index(GuestAddr address) {
    // COM objects are 16-byte aligned or more; a multiplicative hash spreads
    // any allocator stride over the entries.
    return static_cast<usize>((address * 0x9E3779B97F4A7C15ull) >> 56);
}
} // namespace

void D3D11Manager::forget_sampler_memo(GuestAddr address) noexcept {
    SamplerMemoEntry& entry = m_sampler_memo[lookup_memo_index(address)];
    if (entry.address == address) entry = SamplerMemoEntry{};
}

void D3D11Manager::forget_srv_memo(GuestAddr address) noexcept {
    ShaderResourceViewMemoEntry& entry = m_srv_memo[lookup_memo_index(address)];
    if (entry.address == address) entry = ShaderResourceViewMemoEntry{};
}

const D3D11Manager::SamplerState* D3D11Manager::find_sampler_state(
    GuestAddr address) const {
    if (address == 0) return nullptr;
    static_assert(kLookupMemoEntries == 256, "lookup_memo_index yields 8 bits");
    SamplerMemoEntry& entry = m_sampler_memo[lookup_memo_index(address)];
    if (entry.address == address &&
        entry.generation == m_sampler_erase_generation) {
        return entry.state;
    }
    const auto found = m_sampler_states.find(address);
    if (found == m_sampler_states.end()) return nullptr;
    entry = SamplerMemoEntry{address, m_sampler_erase_generation,
                             &found->second};
    return &found->second;
}

const D3D11Manager::ShaderResourceView*
D3D11Manager::find_shader_resource_view(GuestAddr address) const {
    if (address == 0) return nullptr;
    ShaderResourceViewMemoEntry& entry = m_srv_memo[lookup_memo_index(address)];
    if (entry.address == address &&
        entry.generation == m_srv_erase_generation) {
        return entry.view;
    }
    const auto found = m_shader_resource_views.find(address);
    if (found == m_shader_resource_views.end()) return nullptr;
    entry = ShaderResourceViewMemoEntry{address, m_srv_erase_generation,
                                        &found->second};
    return &found->second;
}

namespace {

/// A typed buffer view's size-table entry
/// (graphics::DxbcTranslationOptions::typed_buffer_bias): its element bias,
/// and whether its format lacks alpha, which a load past the view then
/// reads as 1 (D3D11.3 19.1.3.3).
[[nodiscard]] u32 typed_buffer_size_entry(
    const graphics::BackendTextureBuffer& view, u32 dxgi_format) {
    return (view.element_bias() & graphics::kTypedBufferBiasMask) |
           (d3d11::dxgi_format_has_alpha(dxgi_format)
                ? 0u
                : graphics::kTypedBufferAlphaMissing);
}

}  // namespace

u32 D3D11Manager::srv_buffer_size_words(GuestAddr srv_address) const {
    const ShaderResourceView* srv = find_shader_resource_view(srv_address);
    if (srv == nullptr || srv->buffer == 0) return 0;
    if (srv->texture_buffer != nullptr) {
        // A typed buffer view's entry: its element bias and format alpha.
        return typed_buffer_size_entry(*srv->texture_buffer, srv->format);
    }
    const u64 bytes = static_cast<u64>(srv->num_elements) * srv->element_bytes;
    return static_cast<u32>(std::min<u64>(bytes / 4u, 0xFFFFFFFFu));
}

u32 D3D11Manager::uav_buffer_size_words(GuestAddr uav_address) const {
    if (uav_address == 0) return 0;
    const auto uav = m_unordered_access_views.find(uav_address);
    if (uav == m_unordered_access_views.end()) return 0;
    if (uav->second.view_dimension == kD3D11UavDimensionTexture3D) {
        // A W-slice window narrower than its mip (graphics::
        // kVolumeUavWindowSizeShift); the whole mip is bound either way.
        const auto texture = m_textures.find(uav->second.texture);
        if (texture == m_textures.end()) return 0;
        const u32 mip_depth = std::max(
            1u, texture->second.volume_desc.depth >> uav->second.mip_slice);
        if (uav->second.first_w_slice == 0 &&
            uav->second.w_size == mip_depth) {
            return 0;
        }
        return (uav->second.w_size << graphics::kVolumeUavWindowSizeShift) |
               uav->second.first_w_slice;
    }
    if (uav->second.buffer == 0) return 0;
    if (uav->second.texture_buffer != nullptr) {
        // A typed buffer view's entry: its element bias and format alpha.
        return typed_buffer_size_entry(*uav->second.texture_buffer,
                                       uav->second.format);
    }
    const u64 bytes = static_cast<u64>(uav->second.num_elements) *
                      uav->second.element_bytes;
    return static_cast<u32>(std::min<u64>(bytes / 4u, 0xFFFFFFFFu));
}

bool d3d11::fit_view_to_declaration(const ShaderBindingLayout& layout,
                                    bool uav, u32 slot,
                                    graphics::BackendTextureViewDesc& view) {
    if (slot >= (uav ? 64u : static_cast<u32>(SHADER_RESOURCE_SLOTS))) {
        return true;
    }
    const u64 bit = u64{1} << (slot % 64u);
    const bool flat =
        ((uav ? layout.uav_flat : layout.srv_flat[slot / 64u]) & bit) != 0;
    const bool arrayed =
        ((uav ? layout.uav_arrayed : layout.srv_arrayed[slot / 64u]) & bit) !=
        0;
    if (!flat && !arrayed) return true;
    using Dim = graphics::BackendTextureViewDimension;
    switch (view.dimension) {
    case Dim::Automatic:
        return true;
    case Dim::Texture2D:
        if (arrayed) {
            view.dimension = Dim::Texture2DArray;
            view.array_size = 1;
        }
        return true;
    case Dim::Texture2DArray:
        if (flat) {
            view.dimension = Dim::Texture2D;
            view.array_size = 1;
        }
        return true;
    case Dim::Texture2DMS:
    case Dim::Texture2DMSArray:
    case Dim::TextureCube:
    case Dim::TextureCubeArray:
        break;
    }
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED d3d11!*Set%s: a cube or "
                     "multisample view (backend dimension %u) at %c%u, which "
                     "the shader declares Texture%s; not bound\n",
                     uav ? "UnorderedAccessViews" : "ShaderResources",
                     static_cast<unsigned>(view.dimension), uav ? 'u' : 't',
                     slot, flat ? "1D/2D" : "1DArray/2DArray");
    }
    return false;
}

void D3D11Manager::resolve_shader_resource(
    GuestAddr srv_address,
    const graphics::BackendTexture2D*& texture_2d,
    graphics::BackendTextureViewDesc& backend_view,
    const graphics::BackendTexture3D*& texture_3d) {
        const ShaderResourceView* const view =
            find_shader_resource_view(srv_address);
        if (view == nullptr) return;
        const ShaderResourceView& srv_view = *view;
        auto tex = m_textures.find(srv_view.texture);
        if (tex == m_textures.end()) return;
        if (tex->second.volume) {
            if (!tex->second.backend_3d) return;
            // The view's mip window under its format (a typed view of a
            // TYPELESS volume reads that type): measured,
            // d3d11_view_line_probe [vol] "R8G8B8A8_UINT srv".
            // The resource's own format keeps its storage form
            // (texture_storage_format).
            texture_3d = tex->second.backend_3d->view(
                srv_view.format == tex->second.volume_desc.format
                    ? graphics::PixelFormat::Unknown
                    : backend_format(srv_view.format),
                srv_view.most_detailed_mip, srv_view.mip_levels);
            if (texture_3d == nullptr) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED d3d11!Set*Shader"
                                 "Resources: a Texture3D view the backend "
                                 "cannot express (format %u, mips %u+%u)\n",
                                 srv_view.format,
                                 srv_view.most_detailed_mip,
                                 srv_view.mip_levels);
                }
            }
        } else {
            if (!tex->second.backend) return;
            texture_2d = tex->second.backend.get();
            switch (srv_view.view_dimension) {
            // TEXTURE1D / TEXTURE1DARRAY: a height-1 2D texture underneath,
            // which is how the translator declares them too.
            case 2u:
            case 4u:
                backend_view.dimension =
                    graphics::BackendTextureViewDimension::Texture2D;
                break;
            case 3u:
            case 5u:
                backend_view.dimension =
                    graphics::BackendTextureViewDimension::Texture2DArray;
                break;
            // A multisample view of a one-sample texture binds as its 2D
            // mip; the shader reads it through a 2D declaration
            // (single_sample_ms_srv_mask).
            case 6u:
                backend_view.dimension =
                    tex->second.desc.sample_count > 1
                        ? graphics::BackendTextureViewDimension::Texture2DMS
                        : graphics::BackendTextureViewDimension::Texture2D;
                break;
            case 7u:
                backend_view.dimension =
                    tex->second.desc.sample_count > 1
                        ? graphics::BackendTextureViewDimension::
                              Texture2DMSArray
                        : graphics::BackendTextureViewDimension::
                              Texture2DArray;
                break;
            case 9u:
                backend_view.dimension =
                    graphics::BackendTextureViewDimension::TextureCube;
                break;
            case 10u:
                backend_view.dimension =
                    graphics::BackendTextureViewDimension::TextureCubeArray;
                break;
            default:
                backend_view.dimension =
                    graphics::BackendTextureViewDimension::Automatic;
                break;
            }
            backend_view.most_detailed_mip =
                srv_view.most_detailed_mip;
            if (srv_view.format == 22u)
                backend_view.pixel_format =
                    graphics::PixelFormat::X32_Stencil8;
            else if (srv_view.format == 47u)
                backend_view.pixel_format =
                    graphics::PixelFormat::X24_Stencil8;
            else if (!tex->second.backend->desc().depth_stencil &&
                     srv_view.format != 0u) {
                // Learn (D3D11_SHADER_RESOURCE_VIEW_DESC.Format): the view
                // reads the resource AS this format -- an sRGB view of a
                // TYPELESS (or linear) texture decodes sRGB, an R32_UINT view
                // of R32_TYPELESS reads integers. Sampling the storage format
                // instead returned raw UNORM for every sRGB view: UE creates
                // its colour textures typeless with sRGB SRVs, so the whole
                // world sampled washed out (measured with
                // tools/oracle_probe/d3d11_table_texture_views_probe.c).
                const graphics::PixelFormat view_format =
                    backend_format(srv_view.format);
                if (view_format != graphics::PixelFormat::Unknown &&
                    view_format != tex->second.backend->desc().pixel_format) {
                    backend_view.pixel_format = view_format;
                }
            }
            // Components the view format does not store: a depth plane reads
            // (d, 0, 0, 1), B8G8R8X8 alpha reads 1, and B4G4R4A4 is held as
            // ABGR4 (d3d11_format_caps.hpp, view_swizzle_for).
            const bool stencil_plane =
                srv_view.format == 22u || srv_view.format == 47u;
            switch (d3d11::view_swizzle_for(
                        srv_view.format,
                        !stencil_plane &&
                            tex->second.backend->desc().depth_stencil)) {
            case d3d11::ViewSwizzle::OpaqueAlpha:
                backend_view.swizzle = graphics::BackendViewSwizzle::RGB1;
                break;
            case d3d11::ViewSwizzle::Bgra4:
                backend_view.swizzle = graphics::BackendViewSwizzle::GBAR;
                break;
            case d3d11::ViewSwizzle::DepthRed:
                backend_view.swizzle = graphics::BackendViewSwizzle::R001;
                break;
            case d3d11::ViewSwizzle::Identity:
                break;
            }
            backend_view.mip_levels = srv_view.mip_levels;
            backend_view.first_array_slice =
                srv_view.first_array_slice;
            backend_view.array_size = srv_view.array_size;
        }
}

u32 D3D11Manager::rgb32_buffer_srv_mask(std::span<const GuestAddr> views,
                                         bool& unservable) const {
    u32 mask = 0;
    for (usize slot = 0; slot < views.size(); ++slot) {
        if (views[slot] == 0) continue;
        const ShaderResourceView* srv = find_shader_resource_view(views[slot]);
        if (srv == nullptr || srv->buffer == 0 ||
            srv->texture_buffer == nullptr || srv->format < 6u ||
            srv->format > 8u) {
            continue;
        }
        if (slot >= 32u) {
            unservable = true;
            continue;
        }
        mask |= 1u << slot;
    }
    return mask;
}

u32 D3D11Manager::single_sample_ms_srv_mask(std::span<const GuestAddr> views,
                                             bool& unservable) const {
    u32 mask = 0;
    for (usize slot = 0; slot < views.size(); ++slot) {
        if (views[slot] == 0) continue;
        const ShaderResourceView* srv = find_shader_resource_view(views[slot]);
        if (srv == nullptr || srv->texture == 0 ||
            (srv->view_dimension != 6u && srv->view_dimension != 7u)) {
            continue;
        }
        const auto tex = m_textures.find(srv->texture);
        if (tex == m_textures.end() || tex->second.desc.sample_count > 1) {
            continue;
        }
        if (slot >= 32u) {
            unservable = true;
            continue;
        }
        mask |= 1u << slot;
    }
    return mask;
}

graphics::BackendTexture3D* D3D11Manager::volume_uav_binding(
    const UnorderedAccessView& view, Texture2D& texture, u32 slot) {
    if (!texture.backend_3d) return nullptr;
    const u32 mip_depth =
        std::max(1u, texture.volume_desc.depth >> view.mip_slice);
    if ((view.first_w_slice != 0 || view.w_size != mip_depth) &&
        slot >= graphics::kGuardSizeUavSlots) {
        // A W-slice window binds its whole mip and the translated shader
        // moves z by FirstWSlice from its size-table entry
        // (uav_buffer_size_words); a slot past the table has none.
        ++texture.diag_uav_drops;
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED d3d11!Set*UnorderedAccess"
                         "Views: a Texture3D UAV of W slices [%u, +%u) at "
                         "u%u, past the window table; not bound\n",
                         view.first_w_slice, view.w_size, slot);
        }
        return nullptr;
    }
    graphics::BackendTexture3D* bound = texture.backend_3d->view(
        view.format == texture.volume_desc.format
            ? graphics::PixelFormat::Unknown
            : backend_format(view.format),
        view.mip_slice, 1);
    if (bound == nullptr) {
        ++texture.diag_uav_drops;
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED d3d11!Set*UnorderedAccess"
                         "Views: a Texture3D UAV (format %u, mip %u) the "
                         "backend cannot view; not bound\n",
                         view.format, view.mip_slice);
        }
    }
    return bound;
}

void D3D11Manager::fill_typed_buffer_biases(
    std::span<u32> words, std::span<const GuestAddr> stage_srvs) const {
    constexpr usize kTable = graphics::kGuardSizeTableOffset / 4;
    if (words.size() < kTable + graphics::kGuardSizeSrvSlots) return;
    const usize n = std::min<usize>(stage_srvs.size(),
                                    graphics::kGuardSizeSrvSlots);
    for (usize slot = 0; slot < n; ++slot) {
        if (stage_srvs[slot] == 0) continue;
        const ShaderResourceView* srv =
            find_shader_resource_view(stage_srvs[slot]);
        if (srv != nullptr && srv->buffer != 0 &&
            srv->texture_buffer != nullptr) {
            words[kTable + slot] =
                typed_buffer_size_entry(*srv->texture_buffer, srv->format);
        }
    }
}

void D3D11Manager::fill_constant_buffer_limits(
    std::span<u32> words, std::span<const GuestAddr> buffers,
    std::span<const u32> first, std::span<const u32> count) const {
    constexpr usize kBase = graphics::kConstantBufferLimitOffset / 4;
    for (usize slot = 0; slot < buffers.size() &&
                         slot < graphics::kConstantBufferLimitSlots &&
                         kBase + slot < words.size();
         ++slot) {
        u32 readable = 0;
        // An empty slot reads nothing; skip the lookup (R4c).
        const auto buffer = buffers[slot] != 0 ? m_buffers.find(buffers[slot])
                                               : m_buffers.end();
        if (buffers[slot] != 0 && buffer != m_buffers.end()) {
            // D3D11.3 5.3.4.3.2: the shader sees NumConstants elements from
            // FirstConstant ("where 1 means 16 bytes"); any part that falls
            // off the buffer reads as out of bounds, i.e. 0 (7.5). A
            // whole-buffer bind is the window 0 + 4096.
            const u64 elements = buffer->second.desc.byte_width / 16u;
            const u64 start = slot < first.size() ? first[slot] : 0u;
            const u64 window =
                slot < count.size() ? count[slot] : d3d11::WHOLE_CONSTANT_WINDOW;
            if (start < elements) {
                readable = static_cast<u32>(std::min<u64>(
                    std::min<u64>(window, d3d11::WHOLE_CONSTANT_WINDOW),
                    elements - start));
            }
        }
        words[kBase + slot] = graphics::kConstantBufferLimitKnown | readable;
    }
}

u32 D3D11Manager::constant_buffer_bind_offset(GuestAddr buffer,
                                              u32 first) const {
    const auto found = m_buffers.find(buffer);
    if (buffer == 0 || found == m_buffers.end()) return 0;
    const u64 offset = static_cast<u64>(first) * 16u;
    return offset < found->second.desc.byte_width ? static_cast<u32>(offset)
                                                  : 0u;
}

// A t# slot holding a buffer SRV binds a buffer, not a texture — either a
// StructuredBuffer, whose element size is the resource's structure byte
// stride, or a typed buffer, whose element size comes from the view's DXGI
// format. Both are word-addressed by the translated shader, so the element
// window becomes a byte offset from the buffer base.
void D3D11Manager::resolve_buffer_resource(
    GuestAddr srv_address, const graphics::BackendBuffer*& out_buffer,
    unsigned& out_offset,
    const graphics::BackendTextureBuffer*& out_texture_buffer) {
        const ShaderResourceView* srv = find_shader_resource_view(srv_address);
        if (srv == nullptr || srv->buffer == 0) return;
        // A typed view was built as a texture over the buffer's storage, so it
        // binds to the t# texture slot and the shader fetches texels from it.
        if (srv->texture_buffer) {
            out_texture_buffer = srv->texture_buffer.get();
            return;
        }
        auto buffer = m_buffers.find(srv->buffer);
        if (buffer == m_buffers.end() || !buffer->second.backend) return;
        const u32 element_bytes = srv->element_bytes;
        if (element_bytes == 0) return;
        out_buffer = buffer->second.backend.get();
        out_offset = srv->first_element * element_bytes;
}

} // namespace vitreo::winapi
