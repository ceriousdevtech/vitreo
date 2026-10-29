// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_clear.cpp
/// @brief ID3D11DeviceContext view clears and GenerateMips.
///
/// Slots 50 (ClearRenderTargetView), 51 and 52 (ClearUnorderedAccessView
/// Uint / Float), 53 (ClearDepthStencilView) and 54 (GenerateMips), and the
/// clear_unordered_access_view / fill_texture_view_texels members they use.
/// impl_ContextMethod (d3d11_context.cpp) routes exactly these slots here.
///
/// Split out of d3d11_context.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <string>
#include <mutex>
#include <set>
#include <vector>

namespace vitreo::winapi {

bool D3D11Manager::clear_unordered_access_view(
    GuestAddr view_address, const std::array<u32, 4>& values,
    bool is_float_form) {
    auto view = m_unordered_access_views.find(view_address);
    if (view == m_unordered_access_views.end()) return false;
    const auto& uav = view->second;
    std::array<float, 4> color{};
    std::memcpy(color.data(), values.data(), sizeof(color));

    if (uav.buffer != 0) {
        auto buffer = m_buffers.find(uav.buffer);
        if (buffer == m_buffers.end()) return false;
        const bool structured =
            (buffer->second.desc.misc_flags &
             D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0;
        const bool raw = (uav.buffer_flags & d3d11::BUFFER_UAV_FLAG_RAW) != 0;
        std::vector<u8> element;
        if (raw || structured || uav.format == 0) {
            // D3D11.3 5.2.3.2: "For RAW Buffer and Structured Buffer Views,
            // only the first array element's value is used" -- as raw 32-bit
            // words. The Float form "only works on FLOAT, UNORM, and SNORM
            // UAVs ... On other UAVs, the operation is invalid and the call
            // will not reach the driver": the oracle leaves both views as
            // they were.
            if (is_float_form) return false;
            element.resize(sizeof(u32));
            std::memcpy(element.data(), &values[0], sizeof(u32));
        } else {
            // A typed buffer holds one texel of the view's format per
            // element: per component, converted like a texture's.
            const auto* layout = d3d11_texel::layout_for(uav.format);
            if (layout == nullptr) {
                d3d11_unimplemented_once(
                    is_float_form ? "ClearUnorderedAccessViewFloat"
                                  : "ClearUnorderedAccessViewUint",
                    "typed buffer view format " + std::to_string(uav.format));
                return false;
            }
            d3d11_texel::TexelBytes texel{};
            if (is_float_form) {
                if (!d3d11_texel::encode_float(
                        *layout, color,
                        d3d11_texel::FloatClear::UnorderedAccessView, texel)) {
                    return false;  // an integer view: dropped by the runtime
                }
            } else {
                texel = d3d11_texel::encode_uint(*layout, values);
            }
            element.assign(texel.begin(), texel.begin() + layout->texel_bytes);
        }
        const usize element_bytes =
            uav.element_bytes != 0 ? uav.element_bytes : element.size();
        const usize begin =
            static_cast<usize>(uav.first_element) * element_bytes;
        const usize span = static_cast<usize>(uav.num_elements) * element_bytes;
        const usize width = buffer->second.desc.byte_width;
        if (span == 0 || (span % element.size()) != 0 || begin > width ||
            span > width - begin) {
            return false;
        }
        // A tiled buffer keeps no shadow (d3d11_tiled.cpp).
        if (buffer->second.shadow.size() >= begin + span) {
            replicate_element(buffer->second.shadow.data() + begin, span,
                              element);
        }
        return fill_buffer_on_gpu(buffer->second, begin, element, span);
    }

    auto texture = m_textures.find(uav.texture);
    if (texture == m_textures.end()) return false;
    const u32 format = uav.format != 0 ? uav.format : texture->second.desc.format;
    const auto* layout = d3d11_texel::layout_for(format);
    const char* method = is_float_form ? "ClearUnorderedAccessViewFloat"
                                       : "ClearUnorderedAccessViewUint";
    if (layout == nullptr) {
        d3d11_unimplemented_once(method,
                                 "texture view format " + std::to_string(format));
        return false;
    }
    d3d11_texel::TexelBytes texel{};
    if (is_float_form) {
        if (!d3d11_texel::encode_float(
                *layout, color, d3d11_texel::FloatClear::UnorderedAccessView,
                texel)) {
            return false;  // UINT/SINT view: invalid, never reaches a driver
        }
    } else {
        texel = d3d11_texel::encode_uint(*layout, values);
    }
    return fill_texture_view_texels(texture->second, texel.data(),
                                    layout->texel_bytes, uav.mip_slice,
                                    texture->second.volume
                                        ? uav.first_w_slice
                                        : uav.first_array_slice,
                                    texture->second.volume ? uav.w_size
                                                           : uav.array_size,
                                    nullptr, method);
}

bool D3D11Manager::fill_texture_view_texels(
    Texture2D& texture, const u8* texel, u32 texel_bytes, u32 mip,
    u32 first_slice, u32 slice_count, const std::array<u32, 4>* rect,
    const char* method) {
    // The encoded texel must be exactly one storage texel; a format the
    // backend stores wider or narrower than DXGI cannot be filled bit-exact.
    if (texture.volume) {
        if (texture.backend_3d == nullptr) return false;
        const auto& d = texture.backend_3d->desc();
        if (graphics::format_table(d.pixel_format).bytes_per_pixel !=
            texel_bytes) {
            d3d11_unimplemented_once(
                method, "volume storage format differs in size from DXGI " +
                            std::to_string(texture.desc.format));
            return false;
        }
        const u32 w = std::max(1u, d.width >> mip);
        const u32 h = std::max(1u, d.height >> mip);
        const bool ok = texture.backend_3d->fill_texels(
            texel, texel_bytes, mip, 0, 0, first_slice, w, h, slice_count);
        if (!ok) {
            d3d11_unimplemented_once(method,
                                     "backend cannot fill this volume");
        }
        return ok;
    }
    if (texture.backend == nullptr) return false;
    graphics::BackendTexture2D* target = texture.backend.get();
    if (dxgi_format_is_planar(texture.desc.format) &&
        texel_bytes ==
            2u * graphics::format_table(target->desc().pixel_format)
                     .bytes_per_pixel) {
        // A chroma view of a planar video texture (R8G8 / R16G16 / R32_UINT)
        // fills the half-size chroma plane (measured, d3d11_nv12_probe
        // [layout] "uavclear").
        target = target->view_plane(graphics::planar_chroma_format(
            target->desc().pixel_format));
    }
    const auto& d = target->desc();
    if (graphics::format_table(d.pixel_format).bytes_per_pixel != texel_bytes) {
        d3d11_unimplemented_once(
            method, "storage format differs in size from DXGI " +
                        std::to_string(texture.desc.format));
        return false;
    }
    const u32 w = std::max(1u, d.width >> mip);
    const u32 h = std::max(1u, d.height >> mip);
    u32 x = 0, y = 0, width = w, height = h;
    if (rect != nullptr) {
        // ClearView: [left, right) x [top, bottom), clamped to the level.
        x = std::min((*rect)[0], w);
        y = std::min((*rect)[1], h);
        width = std::min((*rect)[2], w) - x;
        height = std::min((*rect)[3], h) - y;
        if ((*rect)[2] <= (*rect)[0] || (*rect)[3] <= (*rect)[1] ||
            width == 0 || height == 0) {
            return true;  // nothing inside the surface
        }
    }
    const bool ok = target->fill_texels(
        texel, texel_bytes, mip, first_slice, slice_count, x, y, width, height);
    if (!ok) {
        d3d11_unimplemented_once(method, "backend cannot fill this texture");
    }
    return ok;
}

void D3D11Manager::context_clear_method(Marshaler& m, usize slot) {
    switch (slot) {
    case 50: {
        // ClearRenderTargetView — Learn IDL slot 50
        const GuestAddr view_address = m.arg_ptr(1);
        const GuestAddr color_address = m.arg_ptr(2);
        auto view = m_render_target_views.find(view_address);
        if (view == m_render_target_views.end() || color_address == 0) {
            d3d11_diag(
                "ID3D11DeviceContext::ClearRenderTargetView invalid "
                "view=0x%llx color=0x%llx",
                static_cast<unsigned long long>(view_address),
                static_cast<unsigned long long>(color_address));
            m.set_return(0);
            return;
        }
        std::array<float, 4> color{};
        if (m_memory.read_bytes(
                color_address, reinterpret_cast<Byte*>(color.data()),
                sizeof(color)).is_err()) {
            m.set_return(0);
            return;
        }
        auto texture = m_textures.find(view->second.texture);
        bool cleared = false;
        // D3D11.3 5.2.3.1: the colour is "converted to the fully qualified
        // format type of the View", by the 3.2 rules: float16/11/10 round
        // toward zero and saturate at the largest finite value, *NORM and
        // sRGB round to nearest-even (measured, a06 probe RTV lines). Handing
        // the backend the exact stored value makes its own conversion land
        // on the same bits.
        const u32 view_format =
            view->second.format != 0 || texture == m_textures.end()
                ? view->second.format
                : texture->second.desc.format;
        const auto* layout = d3d11_texel::layout_for(view_format);
        const std::array<float, 4> clear_color =
            layout != nullptr
                ? d3d11_texel::quantize_render_target_color(*layout, color)
                : color;
        // A NaN bound for an 11/10-bit float channel has one D3D encoding
        // (all ones: a06 probe RTV R11G11B10_FLOAT c5) that no clear colour
        // can request, so that clear stores the encoded texel instead.
        bool store_texel = false;
        if (layout != nullptr) {
            for (u8 i = 0; i < layout->channel_count; ++i) {
                const auto& c = layout->channels[i];
                store_texel = store_texel ||
                    (c.kind == d3d11_texel::ChannelKind::Float &&
                     c.bits < 16 && std::isnan(color[c.component]));
            }
        }
        if (view->second.buffer != 0 && view->second.buffer_proxy) {
            // A buffer view's window, cleared in its proxy and copied back
            // (measured, d3d11_view_line_probe [bufrt] "clear": only the
            // window's elements change).
            auto& proxy = *view->second.buffer_proxy;
            if (store_texel) {
                d3d11_texel::TexelBytes texel{};
                (void)d3d11_texel::encode_float(
                    *layout, color, d3d11_texel::FloatClear::ClearView, texel);
                cleared = proxy.fill_texels(texel.data(), layout->texel_bytes,
                                            0, 0, 1, 0, 0,
                                            view->second.num_elements, 1);
            } else {
                cleared = proxy.clear_render_target_view(
                    clear_color, backend_format(view_format), 0, 0, 1);
            }
            cleared = cleared &&
                      sync_buffer_render_target(view->second, false);
        } else if (store_texel && texture != m_textures.end()) {
            d3d11_texel::TexelBytes texel{};
            (void)d3d11_texel::encode_float(
                *layout, color, d3d11_texel::FloatClear::ClearView, texel);
            cleared = fill_texture_view_texels(
                texture->second, texel.data(), layout->texel_bytes,
                view->second.mip_slice, view->second.first_array_slice,
                view->second.array_size, nullptr, "ClearRenderTargetView");
        } else if (texture != m_textures.end() && texture->second.volume &&
            texture->second.backend_3d) {
            // A Texture3D view clears a window of W slices, not array slices.
            if (backend_format(view_format) !=
                texture->second.backend_3d->desc().pixel_format) {
                d3d11_unimplemented_once(
                    "ClearRenderTargetView",
                    "Texture3D view format " + std::to_string(view_format) +
                        " differs from its storage");
            } else {
                cleared =
                    texture->second.backend_3d->clear_render_target_slices(
                        clear_color, view->second.mip_slice,
                        view->second.first_array_slice,
                        view->second.array_size);
            }
        } else if (texture != m_textures.end() && texture->second.backend) {
            // Prefer the Texture2D backend — including DXGI swapchain back
            // buffers — so Clear and Draw target the same Metal texture that
            // Present blits. A view whose format differs from the storage's
            // (typeless resource, sRGB view of a UNORM back buffer) clears
            // through a view in that format.
            cleared = texture->second.backend->clear_render_target_view(
                clear_color, backend_format(view_format),
                view->second.mip_slice, view->second.first_array_slice,
                view->second.array_size);
        } else if (texture != m_textures.end() &&
                   texture->second.swap_chain_owner != 0) {
            auto swap_chain =
                m_swap_chains.find(texture->second.swap_chain_owner);
            cleared =
                swap_chain != m_swap_chains.end() &&
                swap_chain->second.backend->clear_back_buffer(color);
        }
        if (texture != m_textures.end()) {
            ++(cleared ? texture->second.diag_clears
                       : texture->second.diag_clear_failures);
        }
        d3d11_diag(
            "ID3D11DeviceContext::ClearRenderTargetView view=0x%llx "
            "rgba=(%.3f,%.3f,%.3f,%.3f) -> %s",
            static_cast<unsigned long long>(view_address),
            color[0], color[1], color[2], color[3],
            cleared ? "recorded" : "failed");
        m.set_return(0);
        return;
    }
    case 51:
    case 52: {
        // ClearUnorderedAccessViewUint(pUAV, const UINT[4]) /
        // ClearUnorderedAccessViewFloat(pUAV, const FLOAT[4]).
        //
        // D3D11.3 5.2.3.2: Uint stores the low bits of each value per channel
        // with no conversion (raw/structured buffers: Values[0] as words);
        // Float converts into FLOAT/UNORM/SNORM views and is dropped on any
        // other. Both are recorded on the GPU timeline over the view's exact
        // window (clear_unordered_access_view).
        const bool is_float_form = slot == 52;
        const GuestAddr view_address = m.arg_ptr(1);
        const GuestAddr values_address = m.arg_ptr(2);
        std::array<u32, 4> values{};
        if (values_address != 0) {
            for (u32 i = 0; i < 4; ++i) {
                values[i] = m_memory.read_u32(
                    values_address + i * sizeof(u32)).value_or(0u);
            }
        }
        const bool cleared =
            clear_unordered_access_view(view_address, values, is_float_form);
        if (m_diag_transfer_log != nullptr) [[unlikely]] {
            diag_log_clear_uav(view_address, is_float_form, values, cleared);
        }
        d3d11_diag(
            "ID3D11DeviceContext::ClearUnorderedAccessView%s view=0x%llx "
            "values=(0x%x,0x%x,0x%x,0x%x) -> %s",
            is_float_form ? "Float" : "Uint",
            static_cast<unsigned long long>(view_address),
            values[0], values[1], values[2], values[3],
            cleared ? "cleared" : "ignored");
        m.set_return(0);
        return;
    }
    case 53: {
        // ClearDepthStencilView
        const GuestAddr view_address = m.arg_ptr(1);
        const u32 clear_flags = static_cast<u32>(m.arg(2));
        // FLOAT Depth is the fourth argument: XMM3 on x64, not R9. Reading
        // R9 cleared UE's reversed-Z depth to whatever integer was there
        // (1.0 in practice), so every draw failed its GREATER_EQUAL test and
        // the whole world rendered as sky and fog.
        const float depth = m.arg_f32(3);
        const u8 stencil = static_cast<u8>(m.arg(4));
        auto view = m_depth_stencil_views.find(view_address);
        if (view == m_depth_stencil_views.end()) {
            m.set_return(0);
            return;
        }
        auto texture = m_textures.find(view->second.texture);
        bool cleared = false;
        const bool clear_depth = (clear_flags & 1u) != 0;
        const bool clear_stencil = (clear_flags & 2u) != 0;
        // D3D11.3 5.2.3.1: "Depth clear values outside of the range specified
        // in viewport range will not be passed to the DDI" -- the whole call,
        // stencil included; measured on D16/D24S8/D32/D32S8 for 1.5, -0.5
        // and NaN (a06 probe DSV lines keep the previous contents).
        if (clear_depth && !(depth >= 0.0f && depth <= 1.0f)) {
            d3d11_diag("ID3D11DeviceContext::ClearDepthStencilView "
                       "view=0x%llx depth=%f outside [0,1] -> dropped",
                       static_cast<unsigned long long>(view_address),
                       static_cast<double>(depth));
            m.set_return(0);
            return;
        }
        float clear_value = depth;
        if (texture != m_textures.end() && texture->second.backend &&
            texture->second.backend->desc().pixel_format ==
                graphics::PixelFormat::D16Unorm) {
            // 3.2.3.6 FLOAT -> UNORM rounds to nearest (0.25 -> 0x4000,
            // 0.1 -> 0x199A measured). Hand the backend the code's exact
            // value, nudged a quarter step up so a truncating conversion
            // (Metal's Depth16Unorm clear) lands on it too.
            const u32 code = d3d11_texel::float_to_unorm(depth, 16);
            clear_value = static_cast<float>(
                std::min(1.0, (code + 0.25) / 65535.0));
        }
        if (texture != m_textures.end() && texture->second.backend &&
            (clear_depth || clear_stencil)) {
            cleared = texture->second.backend->clear_depth_stencil_aspects(
                clear_value, stencil, clear_depth, clear_stencil,
                view->second.mip_slice,
                view->second.first_array_slice, view->second.array_size);
        }
        d3d11_diag(
            "ID3D11DeviceContext::ClearDepthStencilView view=0x%llx "
            "flags=0x%x depth=%.3f mip=%u slice=%u count=%u fmt=%u -> %s",
            static_cast<unsigned long long>(view_address), clear_flags, depth,
            view->second.mip_slice, view->second.first_array_slice,
            view->second.array_size,
            texture == m_textures.end() ? 0u : texture->second.desc.format,
            cleared ? "recorded" : "failed");
        m.set_return(0);
        return;
    }
    case 54: {
        // GenerateMips
        const GuestAddr srv = m.arg_ptr(1);
        auto view = m_shader_resource_views.find(srv);
        if (view == m_shader_resource_views.end()) {
            m.set_return(0);
            return;
        }
        auto texture = m_textures.find(view->second.texture);
        bool ok = false;
        const char* outcome = "failed";
        // Learn (GenerateMips): without RENDER_TARGET, SHADER_RESOURCE and
        // MISC_GENERATE_MIPS on the resource the call "has no effect", and on
        // a format outside the MIP_AUTOGEN list (integer formats) it "will
        // silently fail". A one-level view has nothing below its first level.
        // Otherwise only the view's window changes: levels MostDetailedMip+1
        // .. MostDetailedMip+MipLevels-1 of its array slices, each filtered
        // from the level above.
        if (texture != m_textures.end()) {
            const auto& tdesc = texture->second.desc;
            const u32 view_format =
                view->second.format != 0 ? view->second.format : tdesc.format;
            constexpr u32 required_bind =
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            const u32 misc = texture->second.volume
                                 ? texture->second.volume_desc.misc_flags
                                 : tdesc.misc_flags;
            if ((tdesc.bind_flags & required_bind) != required_bind ||
                (misc & D3D11_RESOURCE_MISC_GENERATE_MIPS) == 0) {
                outcome = "no effect (resource lacks GENERATE_MIPS/RT/SRV)";
            } else if (!d3d11_texel::is_filterable_color(view_format)) {
                outcome = "silently failed (format has no MIP_AUTOGEN)";
            } else if (view->second.mip_levels <= 1) {
                ok = true;
                outcome = "no effect (one-level view)";
            } else if (texture->second.volume && texture->second.backend_3d) {
                // A Texture3D view has no array window; its mip window is
                // the whole chain only.
                if (view->second.most_detailed_mip == 0 &&
                    view->second.mip_levels == tdesc.mip_levels) {
                    ok = m_backend.generate_mips(
                        texture->second.backend_3d.get());
                } else {
                    d3d11_unimplemented_once(
                        "GenerateMips", "Texture3D view of a partial mip chain");
                }
            } else if (texture->second.backend) {
                ok = m_backend.generate_mips_window(
                    texture->second.backend.get(),
                    view->second.most_detailed_mip, view->second.mip_levels,
                    view->second.first_array_slice, view->second.array_size);
            }
            if (ok && std::strcmp(outcome, "failed") == 0) outcome = "ok";
        }
        d3d11_diag("GenerateMips srv=0x%llx -> %s",
                   static_cast<unsigned long long>(srv), outcome);
        m.set_return(0);
        return;
    }
    }
}

} // namespace vitreo::winapi
