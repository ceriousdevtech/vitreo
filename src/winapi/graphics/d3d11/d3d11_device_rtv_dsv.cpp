// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_device_rtv_dsv.cpp
/// @brief The output-merger views: ID3D11Device::CreateRenderTargetView
///        and CreateDepthStencilView (vtable slots 9 and 10).
///
/// Split out of d3d11_device_methods.cpp; impl_DeviceMethod forwards the
/// slots here once it has found the device. The view-format rules they
/// share with the other view creators are in d3d11_view_rules.hpp.

#include "core/memory/host_memory_floor.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/d3d11/d3d11_view_rules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

void D3D11Manager::device_rtv_dsv_method(Marshaler& m, usize slot,
                                         GuestAddr self, Device* device) {
    switch (slot) {
    case 9: {
        const GuestAddr resource = m.arg_ptr(1);
        const GuestAddr desc_address = m.arg_ptr(2);
        const GuestAddr view_out = m.arg_ptr(3);
        if (view_out != 0) (void)m.write_ptr(view_out, 0);
        const GuestAddr device_owner = m_interfaces.find(self)->second.owner;
        const auto caps = format_caps_context(*device);
        if (auto buffer = m_buffers.find(resource); buffer != m_buffers.end()) {
            // D3D11_BUFFER_RTV (functional spec §5.3 BUFFER_RENDERTARGETVIEW):
            // {FirstElement, NumElements} in elements of the view format,
            // which must render; a NULL desc names no format and is refused
            // (measured, d3d11_a03_views_probe "rtv buf SR|RT ...").
            if (buffer->second.device_owner != device_owner &&
                view_out != 0) {
                m.set_return(VIEW_OF_FOREIGN_RESOURCE);
                return;
            }
            std::array<u32, 4> words{};
            bool readable = desc_address != 0;
            for (u32 i = 0; readable && i < words.size(); ++i) {
                const auto word = m_memory.read_u32(desc_address + 4 * i);
                readable = word.is_ok();
                words[i] = word.value_or(0);
            }
            const u32 element_bytes = typed_buffer_view_element_bytes(words[0]);
            const u64 elements = element_bytes == 0
                ? 0u : buffer->second.desc.byte_width / element_bytes;
            if ((buffer->second.desc.bind_flags & D3D11_BIND_RENDER_TARGET) ==
                    0 ||
                !readable || words[1] != D3D11_RTV_DIMENSION_BUFFER ||
                !view_format_supports(words[0],
                                      d3d11::FORMAT_SUPPORT_RENDER_TARGET,
                                      caps) ||
                !view_format_supports(words[0], d3d11::FORMAT_SUPPORT_BUFFER,
                                      caps) ||
                words[3] == 0 || words[2] >= elements ||
                words[3] > elements - words[2]) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            if (view_out == 0) {
                m.set_return(1u);
                return;
            }
            // Draws and clears render the window as a NumElements x 1
            // target (measured on WARP, d3d11_view_line_probe [bufrt]: only
            // the window's elements change, at any texel-aligned offset).
            const GuestAddr view = create_buffer_render_target_view(
                device_owner, resource, words[0], words[2], words[3],
                element_bytes);
            if (view == 0) {
                m.set_return(d3d11::E_OUTOFMEMORY);
                return;
            }
            if (m.write_ptr(view_out, view).is_err()) {
                (void)release_object(view);
                m.set_return(d3d11::E_POINTER);
                return;
            }
            m.set_return(d3d11::S_OK);
            return;
        }
        auto texture = m_textures.find(resource);
        if (texture != m_textures.end() && view_out != 0 &&
            texture->second.device_owner != device_owner) {
            m.set_return(VIEW_OF_FOREIGN_RESOURCE);
            return;
        }
        if (texture == m_textures.end()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        u32 format = texture->second.desc.format;
        const bool multisampled = texture->second.desc.sample_count > 1;
        const bool array = texture->second.desc.array_size > 1;
        const bool volume = texture->second.volume;
        const bool linear = texture->second.linear;
        u32 view_dimension = volume
            ? D3D11_RTV_DIMENSION_TEXTURE3D
            : multisampled
            ? (array ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY
                     : D3D11_RTV_DIMENSION_TEXTURE2DMS)
            : linear
            ? (array ? D3D11_RTV_DIMENSION_TEXTURE1DARRAY
                     : D3D11_RTV_DIMENSION_TEXTURE1D)
            : (array ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY
                     : D3D11_RTV_DIMENSION_TEXTURE2D);
        u32 selected_mip = 0;
        u32 first_array_slice = 0;
        u32 array_size = array ? texture->second.desc.array_size : 1u;
        if (volume) {
            // Learn D3D11_TEX3D_RTV: an omitted desc views every W slice of
            // mip 0. WSize is counted in W slices of the *selected mip*.
            array_size = std::max(1u, texture->second.volume_desc.depth);
        }
        const auto rtv_reject = [&](const char* reason, u32 detail) {
            d3d11_diag(
                "ID3D11Device::CreateRenderTargetView REJECT %s (0x%x) "
                "resource=0x%llx fmt=%u %ux%u arr=%u mips=%u samples=%u "
                "volume=%u inferred_dim=%u",
                reason, detail,
                static_cast<unsigned long long>(resource),
                texture->second.desc.format, texture->second.desc.width,
                texture->second.desc.height, texture->second.desc.array_size,
                texture->second.desc.mip_levels,
                texture->second.desc.sample_count, volume ? 1u : 0u,
                view_dimension);
        };
        if ((texture->second.desc.bind_flags &
             D3D11_BIND_RENDER_TARGET) == 0) {
            rtv_reject("bind flags", texture->second.desc.bind_flags);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (desc_address != 0) {
            std::array<u32, 5> words{};
            for (u32 i = 0; i < words.size(); ++i) {
                const auto word = m_memory.read_u32(desc_address + 4 * i);
                if (word.is_err()) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                words[i] = word.value();
            }
            const u32 dimension = words[1];
            // Learn D3D11_RENDER_TARGET_VIEW_DESC: the view dimension is a
            // property of the *view*, not of the resource. Every Texture2D is
            // an array — of one element when ArraySize is 1 — so an engine may
            // name TEXTURE2DARRAY over a single-slice resource, or TEXTURE2D
            // to address one slice of an array. Only the multisample and
            // volume distinctions are resource properties. Demanding the
            // dimension Vitreo inferred refused legal descs, which is fatal to
            // an engine that checks every RTV creation.
            bool dimension_compatible = false;
            switch (dimension) {
            case D3D11_RTV_DIMENSION_TEXTURE3D:
                dimension_compatible = volume;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE1D:
            case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
                // A 1D view names a 1D resource. The backing texture is a
                // height-1 2D one, but that is a host detail the guest must not
                // be able to see through.
                dimension_compatible = linear;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2D:
            case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
                dimension_compatible = !volume && !multisampled && !linear;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2DMS:
            case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
                // Any Texture2D, including a one-sample one, which renders
                // exactly as its single-sample view (measured,
                // d3d11_a03_views_probe "rtv tex2d as 2DMS", "rtv tex2darr as
                // 2DMSARRAY {1,2}").
                dimension_compatible = !volume && !linear;
                break;
            default:
                dimension_compatible = false;
                break;
            }
            if (!dimension_compatible) {
                rtv_reject("dimension", dimension);
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            view_dimension = dimension;
            const bool view_multisampled =
                view_dimension == D3D11_RTV_DIMENSION_TEXTURE2DMS ||
                view_dimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
            // A single-slice view addresses slice 0 whatever the resource's
            // array size; an array view takes its window from the desc below.
            if (view_dimension == D3D11_RTV_DIMENSION_TEXTURE2D ||
                view_dimension == D3D11_RTV_DIMENSION_TEXTURE2DMS ||
                view_dimension == D3D11_RTV_DIMENSION_TEXTURE1D) {
                first_array_slice = 0;
                array_size = 1;
            }
            // The MS forms have no MipSlice.
            if (view_multisampled) {
                selected_mip = 0;
            } else {
                if (words[2] >= texture->second.desc.mip_levels) {
                    rtv_reject("mip slice", words[2]);
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                selected_mip = words[2];
            }
            if (view_dimension == D3D11_RTV_DIMENSION_TEXTURE3D) {
                // D3D11_TEX3D_RTV: MipSlice at +8, FirstWSlice at +12,
                // WSize at +16. The window is bounded by the mip's depth,
                // which halves per level like every other volume dimension.
                const u32 mip_depth = std::max(
                    1u, texture->second.volume_desc.depth >> selected_mip);
                // Learn: WSize of -1 selects every remaining slice.
                const u32 requested_count = words[4] == 0xFFFFFFFFu
                    ? mip_depth - std::min(words[3], mip_depth)
                    : words[4];
                if (requested_count == 0 || words[3] >= mip_depth ||
                    requested_count > mip_depth - words[3]) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                first_array_slice = words[3];
                array_size = requested_count;
            }
            // D3D11_TEX1D_ARRAY_RTV and D3D11_TEX2D_ARRAY_RTV place
            // {FirstArraySlice, ArraySize} after MipSlice; the MS form has no
            // MipSlice. An ArraySize of -1 is every remaining slice (measured,
            // d3d11_view_desc_probe / d3d11_view_window_probe).
            if (view_dimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY ||
                view_dimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY ||
                view_dimension == D3D11_RTV_DIMENSION_TEXTURE1DARRAY) {
                const u32 slices = texture->second.desc.array_size;
                const u32 first = view_multisampled ? words[2] : words[3];
                u32 size_value = view_multisampled ? words[3] : words[4];
                if (size_value == 0xFFFFFFFFu && first < slices) {
                    size_value = slices - first;
                }
                if (size_value == 0 || first >= slices ||
                    size_value > slices - first) {
                    rtv_reject("array window",
                               ((first & 0xFFFFu) << 16) |
                                   (size_value & 0xFFFFu));
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                first_array_slice = first;
                array_size = size_value;
            }
            if (words[0] != 0) format = words[0];
        }
        // Learn: typeless resources may be viewed as a typed member of the
        // same DXGI format family (e.g. R8G8B8A8_TYPELESS → *_UNORM).
        // DXGI swap-chain back buffers are the documented exception to
        // the usual typed-resource view rule: an *_UNORM swap-chain
        // buffer may be viewed through the matching *_UNORM_SRGB RTV.
        // (Microsoft Learn, "Converting data for the color space".)
        const bool swap_chain_srgb_view =
            texture->second.swap_chain_owner != 0 &&
            ((texture->second.desc.format == 28u && format == 29u) ||
             (texture->second.desc.format == 87u && format == 91u));
        // The format must also be one a render target can hold
        // (view_format_supports: RENDER_TARGET), which refuses a typeless
        // resource's NULL desc and every D* format (measured,
        // d3d11_a03_views_probe "rtv TYPELESS res NULL desc", "rtv R32_TL(RT)
        // res as D32_FLOAT").
        const bool video_resource =
            dxgi_format_has_video_views(texture->second.desc.format);
        if ((video_resource
                 ? dxgi_video_view_plane(texture->second.desc.format, format,
                                         false) < 0
                 : format != texture->second.desc.format &&
                       !dxgi_typeless_view_compatible(
                           texture->second.desc.format, format) &&
                       !swap_chain_srgb_view) ||
            !view_format_supports(format, d3d11::FORMAT_SUPPORT_RENDER_TARGET,
                                  caps)) {
            rtv_reject("view format", format);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const GuestAddr view = create_render_target_view(
            texture->second.device_owner, resource, format,
            view_dimension, selected_mip, first_array_slice, array_size);
        if (view == 0) {
            rtv_reject("creation", (first_array_slice << 16) | array_size);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (view_out == 0) {
            (void)release_object(view);
            m.set_return(1u);
            return;
        }
        if (m.write_ptr(view_out, view).is_err()) {
            (void)release_object(view);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag(
            "ID3D11Device::CreateRenderTargetView resource=0x%llx "
            "-> 0x%llx",
            static_cast<unsigned long long>(resource),
            static_cast<unsigned long long>(view));
        m.set_return(d3d11::S_OK);
        return;
    }
    case 10: {
        // CreateDepthStencilView
        const GuestAddr resource = m.arg_ptr(1);
        const GuestAddr desc_address = m.arg_ptr(2);
        const GuestAddr view_out = m.arg_ptr(3);
        if (view_out != 0) (void)m.write_ptr(view_out, 0);
        const GuestAddr device_owner = m_interfaces.find(self)->second.owner;
        auto texture = m_textures.find(resource);
        if (texture != m_textures.end() && view_out != 0 &&
            texture->second.device_owner != device_owner) {
            m.set_return(VIEW_OF_FOREIGN_RESOURCE);
            return;
        }
        if (texture == m_textures.end()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // Learn D3D11_DEPTH_STENCIL_VIEW_DESC, measured (d3d11_dsv_probe): the
        // dimension belongs to the view, within the resource's kind. Over a
        // Texture2D, single or array, TEXTURE2D (one slice) or
        // TEXTURE2DARRAY (a window); over any Texture2D -- a one-sample one
        // too (d3d11_a03_views_probe "dsv D32 (1 sample) as 2DMS") --
        // TEXTURE2DMS or TEXTURE2DMSARRAY, and a multisampled one takes only
        // those; over a Texture1D TEXTURE1D or TEXTURE1DARRAY. A NULL desc
        // picks the array form exactly when there is more than one slice.
        // ArraySize -1 is every remaining slice.
        u32 format = texture->second.desc.format;
        const u32 slices = texture->second.desc.array_size;
        const bool array = slices > 1;
        const bool linear = texture->second.linear;
        const bool multisampled = texture->second.desc.sample_count > 1;
        constexpr u32 DSV_1D = 1, DSV_1DARRAY = 2, DSV_2D = 3, DSV_2DARRAY = 4,
                      DSV_2DMS = 5, DSV_2DMSARRAY = 6;
        u32 view_dimension = linear ? (array ? DSV_1DARRAY : DSV_1D)
            : multisampled ? (array ? DSV_2DMSARRAY : DSV_2DMS)
                           : (array ? DSV_2DARRAY : DSV_2D);
        u32 flags = 0;
        u32 mip_slice = 0;
        u32 first_array_slice = 0;
        u32 array_size = slices;
        u32 union_tail = 0;
        if (desc_address != 0) {
            std::array<u32, 6> words{};
            for (u32 i = 0; i < words.size(); ++i) {
                const auto word = m_memory.read_u32(desc_address + 4 * i);
                if (word.is_err()) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
                words[i] = word.value();
            }
            const u32 dimension = words[1];
            const bool ms_dimension =
                dimension == DSV_2DMS || dimension == DSV_2DMSARRAY;
            const bool compatible = linear
                ? (dimension == DSV_1D || dimension == DSV_1DARRAY)
                : ms_dimension ||
                  (!multisampled &&
                   (dimension == DSV_2D || dimension == DSV_2DARRAY));
            if (!compatible) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            if (words[0] != 0) format = words[0];
            view_dimension = dimension;
            flags = words[2];
            switch (dimension) {
            case DSV_1D:
                mip_slice = words[3];
                array_size = 1;
                union_tail = 1;
                break;
            case DSV_2D:
                mip_slice = words[3];
                array_size = 1;
                break;
            case DSV_1DARRAY:
            case DSV_2DARRAY:
                mip_slice = words[3];
                first_array_slice = words[4];
                array_size = words[5];
                break;
            case DSV_2DMS:
                array_size = 1;
                break;
            case DSV_2DMSARRAY:
                first_array_slice = words[3];
                array_size = words[4];
                break;
            default:
                break;
            }
            if (array_size == 0xFFFFFFFFu && first_array_slice < slices) {
                array_size = slices - first_array_slice;
            }
        }
        // The view format must be a depth-stencil format of the resource's
        // family (view_format_supports: DEPTH_STENCIL -- D32_FLOAT_S8X24_UINT,
        // D32_FLOAT, D24_UNORM_S8_UINT and D16_UNORM).
        const bool format_compatible =
            format == texture->second.desc.format ||
            dxgi_typeless_view_compatible(texture->second.desc.format, format);
        const bool depth_view_format = view_format_supports(
            format, d3d11::FORMAT_SUPPORT_DEPTH_STENCIL,
            format_caps_context(*device));
        // Learn D3D11_DSV_FLAG: READ_ONLY_STENCIL "should only be used if the
        // format includes stencil" -- refused on D32_FLOAT and D16_UNORM
        // (measured, d3d11_a03_views_probe "dsv D32/D16 flags RO_STENCIL").
        const bool stencil_format = format == 20u || format == 45u;
        const u32 valid_flags = d3d11::DSV_READ_ONLY_DEPTH |
                                d3d11::DSV_READ_ONLY_STENCIL;
        if (!depth_view_format || !format_compatible ||
            (flags & ~valid_flags) != 0 ||
            ((flags & d3d11::DSV_READ_ONLY_STENCIL) != 0 && !stencil_format) ||
            mip_slice >= texture->second.desc.mip_levels || array_size == 0 ||
            first_array_slice >= texture->second.desc.array_size ||
            array_size > texture->second.desc.array_size - first_array_slice ||
            !texture->second.backend ||
            backend_format(format) !=
                texture->second.backend->desc().pixel_format) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const GuestAddr view = create_depth_stencil_view(
            texture->second.device_owner, resource, format, view_dimension,
            flags, mip_slice, first_array_slice, array_size);
        if (view == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        m_depth_stencil_views[view].union_tail = union_tail;
        if (view_out == 0) {
            (void)release_object(view);
            m.set_return(1u);
            return;
        }
        if (m.write_ptr(view_out, view).is_err()) {
            (void)release_object(view);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi
