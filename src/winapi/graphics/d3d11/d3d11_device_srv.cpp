// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_device_srv.cpp
/// @brief ID3D11Device::CreateShaderResourceView (vtable slot 7).
///
/// Split out of d3d11_device_methods.cpp; impl_DeviceMethod forwards the
/// slot here once it has found the device. The view-format rules it
/// shares with the other view creators are in d3d11_view_rules.hpp.

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

void D3D11Manager::device_srv_method(Marshaler& m, usize slot, GuestAddr self,
                                     Device* device) {
    switch (slot) {
    case 7: {
        const GuestAddr resource = m.arg_ptr(1);
        const GuestAddr desc_address = m.arg_ptr(2);
        const GuestAddr view_out = m.arg_ptr(3);
        if (view_out != 0) (void)m.write_ptr(view_out, 0);
        const GuestAddr device_owner = m_interfaces.find(self)->second.owner;
        const auto caps = format_caps_context(*device);
        // A t# register names either a texture or a buffer. Learn
        // (D3D11_SHADER_RESOURCE_VIEW_DESC): Format at +0, ViewDimension at
        // +4, and for BUFFER/BUFFEREX the union at +8 holds FirstElement and
        // NumElements (BUFFEREX adds Flags at +16). A StructuredBuffer view
        // uses DXGI_FORMAT_UNKNOWN and counts elements of the buffer's
        // structure byte stride.
        const auto srv_reject = [&](const char* why) {
            u32 words[8] = {};
            for (int i = 0; i < 8; ++i) {
                words[i] = desc_address == 0
                    ? 0
                    : m_memory.read_u32(desc_address +
                                        static_cast<u64>(i) * 4)
                          .value_or(0xFFFFFFFFu);
            }
            std::fprintf(
                stderr,
                "[srv-reject] %s resource=0x%llx desc=%u "
                "%u %u %u %u %u %u %u %u\n",
                why, static_cast<unsigned long long>(resource),
                desc_address == 0 ? 0u : 1u, words[0], words[1], words[2],
                words[3], words[4], words[5], words[6], words[7]);
            std::fflush(stderr);
            m.set_return(d3d11::E_INVALIDARG);
        };
        if (auto buffer = m_buffers.find(resource); buffer != m_buffers.end()) {
            if (buffer->second.device_owner != device_owner &&
                view_out != 0) {
                m.set_return(VIEW_OF_FOREIGN_RESOURCE);
                return;
            }
            const u32 stride = buffer->second.desc.structure_byte_stride;
            if ((buffer->second.desc.bind_flags &
                 D3D11_BIND_SHADER_RESOURCE) == 0) {
                std::fprintf(
                    stderr,
                    "[srv-reject] buffer-check bind=0x%x bytes=%u stride=%u "
                    "misc=0x%x desc=%u\n",
                    buffer->second.desc.bind_flags,
                    buffer->second.desc.byte_width, stride,
                    buffer->second.desc.misc_flags,
                    desc_address == 0 ? 0u : 1u);
                auto created = m_buffer_create_callers.find(resource);
                std::fprintf(
                    stderr,
                    "[srv-reject] caller=0x%llx current buffer created by "
                    "0x%llx\n",
                    static_cast<unsigned long long>(m.guest_return_address()),
                    static_cast<unsigned long long>(
                        created != m_buffer_create_callers.end()
                            ? created->second : 0));
                if (auto tomb = m_buffer_tombstones.find(resource);
                    tomb != m_buffer_tombstones.end()) {
                    std::fprintf(
                        stderr,
                        "[srv-reject] slot previously held a destroyed buffer "
                        "bytes=%u bind=0x%x misc=0x%x stride=%u created by "
                        "0x%llx, final Release by 0x%llx\n",
                        tomb->second.desc.byte_width,
                        tomb->second.desc.bind_flags,
                        tomb->second.desc.misc_flags,
                        tomb->second.desc.structure_byte_stride,
                        static_cast<unsigned long long>(
                            tomb->second.create_caller),
                        static_cast<unsigned long long>(
                            tomb->second.final_release_caller));
                }
                std::fflush(stderr);
                srv_reject("buffer-bind");
                return;
            }
            u32 requested_format = 0;
            u32 dimension = D3D11_SRV_DIMENSION_BUFFER;
            u32 first_element = 0;
            u32 num_elements = 0;
            u32 buffer_flags = 0;
            if (desc_address == 0) {
                // Learn (CreateShaderResourceView): a NULL desc views the
                // whole resource. A structured buffer is the one buffer that
                // names its elements without a desc -- {UNKNOWN, BUFFER, 0,
                // ByteWidth / StructureByteStride}; a typed or raw buffer
                // needs the format only a desc supplies (measured,
                // d3d11_a03_views_probe "srv buf structured/typed/raw NULL
                // desc", d3d11_view_desc_probe).
                if (stride == 0) {
                    srv_reject("buffer-null-desc");
                    return;
                }
                num_elements = buffer->second.desc.byte_width / stride;
            } else {
                std::array<u32, 5> words{};
                for (u32 i = 0; i < words.size(); ++i) {
                    const auto word = m_memory.read_u32(desc_address + 4 * i);
                    if (word.is_err()) {
                        srv_reject("buffer-desc-read");
                        return;
                    }
                    words[i] = word.value();
                }
                requested_format = words[0];
                dimension = words[1];
                first_element = words[2];
                num_elements = words[3];
                // Learn D3D11_BUFFEREX_SRV: Flags at +16. RAW (0x1) is the
                // one flag with a meaning; Windows accepts any other bit and
                // GetDesc echoes the word (measured, "srv buf typed BUFFEREX
                // flags2").
                if (dimension == D3D11_SRV_DIMENSION_BUFFEREX) {
                    buffer_flags = words[4];
                }
            }
            if (dimension != D3D11_SRV_DIMENSION_BUFFER &&
                dimension != D3D11_SRV_DIMENSION_BUFFEREX) {
                srv_reject("buffer-dimension");
                return;
            }
            // A RAW view makes the SRV a ByteAddressBuffer. It has no element
            // type at all -- it is byte-addressed and read as 32-bit words --
            // so it requires DXGI_FORMAT_R32_TYPELESS, a resource created
            // with ALLOW_RAW_VIEWS, and it counts its window in 4-byte units.
            constexpr u32 kBufferExSrvFlagRaw = 0x1u;
            const bool raw_view = (buffer_flags & kBufferExSrvFlagRaw) != 0;
            // Measured (d3d11_raw_view_alignment_probe): a raw view's
            // FirstElement must be a multiple of 4 -- a 16-byte-aligned offset.
            if (raw_view &&
                ((buffer->second.desc.misc_flags &
                  D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) == 0 ||
                 requested_format != 39u || (first_element & 3u) != 0)) {
                srv_reject("raw-view");
                return;
            }
            if (!raw_view && stride != 0 && requested_format != 0) {
                // A structured view's elements are the resource's structures,
                // so it names no format (measured, "srv buf struct BUFFER
                // R32_FLOAT" is E_INVALIDARG).
                srv_reject("structured-typed-format");
                return;
            }
            // A structured buffer names its element size in the resource; a
            // typed buffer names it in the view's DXGI format, which must be
            // a Buffer<> format (view_format_supports: BUFFER). Learn
            // (D3D11_BUFFER_SRV): FirstElement and NumElements count elements
            // of exactly that size in both cases.
            u32 element_bytes = raw_view ? 4u : stride;
            if (element_bytes == 0) {
                if (!view_format_supports(requested_format,
                                          d3d11::FORMAT_SUPPORT_BUFFER, caps)) {
                    srv_reject("typed-buffer-format");
                    return;
                }
                element_bytes = typed_buffer_view_element_bytes(
                    requested_format);
                if (element_bytes == 0) {
                    d3d11_unimplemented_once(
                        "CreateShaderResourceView",
                        "a typed buffer format the capability table reports "
                        "but no texture buffer can hold");
                    m.set_return(d3d11::E_NOTIMPL);
                    return;
                }
            }
            const u64 elements = buffer->second.desc.byte_width / element_bytes;
            if (num_elements == 0 || first_element >= elements ||
                num_elements > elements - first_element) {
                srv_reject("buffer-window");
                return;
            }
            if (view_out == 0) {
                m.set_return(1u);
                return;
            }
            // A typed view aliases the buffer's storage as a texture so the
            // hardware performs the format decode; a StructuredBuffer view
            // stays a buffer, because its elements are opaque structs.
            // R32G32B32 has no Metal texel: its elements alias as three
            // texels of the matching R32 format each, which the shader reads
            // back as one element (DxbcTranslationOptions::rgb32_buffer_srvs,
            // rgb32_buffer_srv_mask).
            std::shared_ptr<graphics::BackendTextureBuffer> texture_buffer;
            if (stride == 0 && !raw_view) {
                if (!buffer->second.backend) {
                    m.set_return(d3d11::E_FAIL);
                    return;
                }
                const bool rgb32 =
                    requested_format >= 6u && requested_format <= 8u;
                const graphics::PixelFormat texel_format = !rgb32
                    ? backend_format(requested_format)
                    : requested_format == 6u ? graphics::PixelFormat::R32Float
                    : requested_format == 7u ? graphics::PixelFormat::R32Uint
                                             : graphics::PixelFormat::R32Sint;
                const u32 texels_per_element = rgb32 ? 3u : 1u;
                texture_buffer = m_backend.create_texture_buffer(
                    *buffer->second.backend, texel_format,
                    first_element * texels_per_element,
                    num_elements * texels_per_element);
                if (!texture_buffer) {
                    if (buffer->second.tiled) {
                        // Metal has no texture view of a placement sparse
                        // buffer (measured: nil).
                        d3d11_unimplemented_once(
                            "CreateShaderResourceView",
                            "typed view of a tiled buffer");
                    }
                    d3d11_diag("ID3D11Device::CreateShaderResourceView "
                               "buffer=0x%llx -> E_FAIL (no texture view) "
                               "fmt=%u first=%u count=%u",
                               static_cast<unsigned long long>(resource),
                               requested_format, first_element, num_elements);
                    m.set_return(d3d11::E_FAIL);
                    return;
                }
            }
            const GuestAddr view = create_buffer_shader_resource_view(
                buffer->second.device_owner, resource, requested_format,
                dimension, first_element, num_elements, element_bytes,
                std::move(texture_buffer));
            if (view == 0) {
                m.set_return(d3d11::E_FAIL);
                return;
            }
            m_shader_resource_views[view].buffer_flags = buffer_flags;
            if (m.write_ptr(view_out, view).is_err()) {
                (void)release_object(view);
                m.set_return(d3d11::E_POINTER);
                return;
            }
            d3d11_diag(
                "ID3D11Device::CreateShaderResourceView buffer=0x%llx -> "
                "0x%llx element=%u stride=%u first=%u count=%u",
                static_cast<unsigned long long>(resource),
                static_cast<unsigned long long>(view), element_bytes, stride,
                first_element, num_elements);
            m.set_return(d3d11::S_OK);
            return;
        }
        auto texture = m_textures.find(resource);
        if (texture != m_textures.end() && view_out != 0 &&
            texture->second.device_owner != device_owner) {
            m.set_return(VIEW_OF_FOREIGN_RESOURCE);
            return;
        }
        if (texture == m_textures.end() ||
            (texture->second.desc.bind_flags &
             D3D11_BIND_SHADER_RESOURCE) == 0) {
            srv_reject("texture-missing-or-unbindable");
            return;
        }

        u32 format = texture->second.desc.format;
        const bool volume = texture->second.volume;
        const bool linear = texture->second.linear;
        const bool cube =
            (texture->second.desc.misc_flags &
             D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
        const bool multisampled = texture->second.desc.sample_count > 1;
        const u32 resource_array_size = texture->second.desc.array_size;
        const u32 resource_mips = texture->second.desc.mip_levels;
        // Learn (D3D11_SHADER_RESOURCE_VIEW_DESC): ViewDimension names the
        // resource type of the view and selects the union member that carries
        // the mip window and, for the array forms, the slice window. A NULL
        // desc means "the whole resource, in the resource's own format", so
        // the default dimension below is derived from the resource. A supplied
        // desc chooses any dimension of the same resource type: an array
        // dimension does not require the resource to have more than one slice,
        // so a one-slice Texture2D is legally viewed as TEXTURE2DARRAY with
        // ArraySize == 1, and a six-face TEXTURECUBE as TEXTURECUBEARRAY with
        // NumCubes == 1.
        u32 view_dimension =
            volume ? D3D11_SRV_DIMENSION_TEXTURE3D
            : multisampled
                ? (resource_array_size > 1
                       ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY
                       : D3D11_SRV_DIMENSION_TEXTURE2DMS)
            : cube ? (resource_array_size > 6
                          ? D3D11_SRV_DIMENSION_TEXTURECUBEARRAY
                          : D3D11_SRV_DIMENSION_TEXTURECUBE)
            : linear ? (resource_array_size > 1
                            ? D3D11_SRV_DIMENSION_TEXTURE1DARRAY
                            : D3D11_SRV_DIMENSION_TEXTURE1D)
            : resource_array_size > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY
                                      : D3D11_SRV_DIMENSION_TEXTURE2D;
        u32 most_detailed_mip = 0;
        u32 mip_levels = resource_mips;
        u32 first_array_slice = 0;
        u32 array_size =
            (view_dimension == D3D11_SRV_DIMENSION_TEXTURE1D ||
             view_dimension == D3D11_SRV_DIMENSION_TEXTURE2D ||
             view_dimension == D3D11_SRV_DIMENSION_TEXTURE2DMS ||
             view_dimension == D3D11_SRV_DIMENSION_TEXTURE3D)
                ? 1u : resource_array_size;
        // -1 is every remaining slice (measured, d3d11_view_window_probe);
        // a window that starts past the end stays unresolved and is refused
        // below.
        const auto remaining_slices = [&](u32 first, u32 size) {
            return size == 0xFFFFFFFFu && first < resource_array_size
                ? resource_array_size - first
                : size;
        };
        if (desc_address != 0) {
            std::array<u32, 6> words{};
            for (u32 i = 0; i < words.size(); ++i) {
                const auto word = m_memory.read_u32(desc_address + 4 * i);
                if (word.is_err()) {
                    srv_reject("texture-desc-read");
                    return;
                }
                words[i] = word.value();
            }
            const u32 requested_dimension = words[1];
            bool dimension_compatible = false;
            switch (requested_dimension) {
            case D3D11_SRV_DIMENSION_TEXTURE3D:
                dimension_compatible = volume;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE1D:
            case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
                // A 1D view names a 1D resource. The backing texture is a
                // height-1 2D one, but that is a host detail -- a guest that
                // asked for a Texture1D and gets a 2D view accepted would bind
                // it to a `Texture2D` shader register that cannot address it.
                dimension_compatible = linear;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2D:
            case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
                dimension_compatible = !volume && !multisampled && !linear;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2DMS:
            case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
                // Any Texture2D, including a one-sample one (measured,
                // d3d11_a03_views_probe "srv tex2d as 2DMS",
                // "srv tex2d as 2DMSARRAY {0,1}"); a multisampled resource
                // takes only these two.
                dimension_compatible = !volume && !linear;
                break;
            case D3D11_SRV_DIMENSION_TEXTURECUBE:
            case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
                // A cube view requires the resource to have been created with
                // D3D11_RESOURCE_MISC_TEXTURECUBE; face ordering is a resource
                // property, not a view property.
                dimension_compatible = !volume && !multisampled && cube;
                break;
            default:
                dimension_compatible = false;
                break;
            }
            if (!dimension_compatible) {
                d3d11_diag(
                    "ID3D11Device::CreateShaderResourceView REJECT "
                    "resource=0x%llx dimension=%u (resource arr=%u samples=%u "
                    "cube=%u volume=%u)",
                    static_cast<unsigned long long>(resource),
                    requested_dimension, resource_array_size,
                    texture->second.desc.sample_count,
                    cube ? 1u : 0u, volume ? 1u : 0u);
                srv_reject("texture-dimension");
                return;
            }
            view_dimension = requested_dimension;
            if (words[0] != 0) format = words[0];
            switch (view_dimension) {
            case D3D11_SRV_DIMENSION_TEXTURE2DMS:
                // D3D11_TEX2DMS_SRV has no window: slice 0, the one "mip".
                most_detailed_mip = 0;
                mip_levels = 1;
                first_array_slice = 0;
                array_size = 1;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
                // D3D11_TEX2DMS_ARRAY_SRV {FirstArraySlice, ArraySize} at the
                // head of the union; ArraySize -1 resolves as for every other
                // array (measured, "srv tex2dms(arr) as 2DMSARRAY {..,-1}").
                most_detailed_mip = 0;
                mip_levels = 1;
                first_array_slice = words[2];
                array_size = remaining_slices(words[2], words[3]);
                break;
            default: {
                most_detailed_mip = words[2];
                if (most_detailed_mip >= resource_mips) {
                    srv_reject("mip-past-end");
                    return;
                }
                mip_levels = words[3] == 0xFFFFFFFFu
                    ? resource_mips - most_detailed_mip
                    : words[3];
                if (view_dimension == D3D11_SRV_DIMENSION_TEXTURE1DARRAY ||
                    view_dimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
                    first_array_slice = words[4];
                    array_size = remaining_slices(words[4], words[5]);
                } else if (view_dimension ==
                           D3D11_SRV_DIMENSION_TEXTURECUBEARRAY) {
                    // D3D11_TEXCUBE_ARRAY_SRV {First2DArrayFace, NumCubes}:
                    // "Index of the first 2D texture to use" (Learn) -- any
                    // face, not only one that starts a cube -- and NumCubes
                    // whole cubes that must fit after it (functional spec
                    // §5.3, "First2DArrayFace <= ArraySize - 6"). NumCubes -1
                    // takes every whole cube left; none left is refused
                    // (measured, d3d11_a03_views_probe "srv cubearr CUBEARRAY
                    // {0,-1,3,1}", "{3,-1}", "{6,-1}", "{7,-1}").
                    first_array_slice = words[4];
                    u32 cubes = words[5];
                    if (cubes == 0xFFFFFFFFu) {
                        cubes = first_array_slice < resource_array_size
                            ? (resource_array_size - first_array_slice) / 6u
                            : 0u;
                    }
                    if (cubes == 0 ||
                        cubes > std::numeric_limits<u32>::max() / 6u) {
                        srv_reject("cube-array");
                        return;
                    }
                    array_size = cubes * 6u;
                } else if (view_dimension ==
                           D3D11_SRV_DIMENSION_TEXTURECUBE) {
                    first_array_slice = 0;
                    array_size = 6;
                } else {
                    first_array_slice = 0;
                    array_size = 1;
                }
                break;
            }
            }
        }
        // D3D11.3 functional spec §5.3: the view format is fully qualified
        // (NULL desc or UNKNOWN resolve to the resource's own, which a
        // typeless resource cannot supply), a member of the resource's
        // family, and one a shader can read -- SHADER_LOAD or SHADER_SAMPLE,
        // which no D* format has (measured, d3d11_a03_views_probe "srv
        // TYPELESS res NULL desc", "srv R24G8_TL res as D24_UNORM_S8_UINT").
        // A video resource is viewed through its element formats only -- a
        // planar one a plane at a time (Learn DXGI_FORMAT NV12 / P010 /
        // P016 / AYUV / Y410 / Y416 / YUY2 / Y210 / Y216; measured,
        // d3d11_nv12_probe and d3d11_video_formats_probe [views]: a NULL
        // desc, UNKNOWN and the resource's own format are E_INVALIDARG).
        if (dxgi_yuy2_rgb_view(texture->second.desc.format, format)) {
            d3d11_unimplemented_once(
                "CreateShaderResourceView",
                "R8G8_B8G8_UNORM view of a DXGI_FORMAT_YUY2 texture (no "
                "Metal texel format views the half-width storage that way)");
            srv_reject("view-format");
            return;
        }
        const bool video =
            dxgi_format_has_video_views(texture->second.desc.format);
        if ((video
                 ? dxgi_video_view_plane(texture->second.desc.format, format,
                                         false) < 0
                 : format != texture->second.desc.format &&
                       !dxgi_typeless_view_compatible(
                           texture->second.desc.format, format)) ||
            !view_format_supports(format,
                                  d3d11::FORMAT_SUPPORT_SHADER_LOAD |
                                      d3d11::FORMAT_SUPPORT_SHADER_SAMPLE,
                                  caps)) {
            srv_reject("view-format");
            return;
        }
        // A mip or slice window past the resource is a malformed desc, which
        // Windows refuses as such (measured, d3d11_view_window_probe).
        if (mip_levels == 0 ||
            mip_levels > resource_mips - most_detailed_mip ||
            array_size == 0 ||
            first_array_slice >= resource_array_size ||
            array_size > resource_array_size - first_array_slice) {
            srv_reject("window");
            return;
        }
        // A TEXTURE2DMS(ARRAY) view of a one-sample texture is legal and
        // binds as its 2D mip, read by a translator variant
        // (single_sample_ms_srv_mask).
        if (view_out == 0) {
            m.set_return(1u);
            return;
        }
        const GuestAddr view = create_shader_resource_view(
            texture->second.device_owner, resource, format,
            view_dimension, most_detailed_mip, mip_levels,
            first_array_slice, array_size);
        if (view == 0) {
            m.set_return(d3d11::E_FAIL);
            return;
        }
        if (desc_address != 0 &&
            view_dimension == D3D11_SRV_DIMENSION_TEXTURE2DMS) {
            m_shader_resource_views[view].ms_unused_field =
                m_memory.read_u32(desc_address + 8).value_or(0);
        }
        if (m.write_ptr(view_out, view).is_err()) {
            (void)release_object(view);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag(
            "ID3D11Device::CreateShaderResourceView "
            "resource=0x%llx -> 0x%llx fmt=%u",
            static_cast<unsigned long long>(resource),
            static_cast<unsigned long long>(view), format);
        m.set_return(d3d11::S_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi
