// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/d3d11/d3d11_format_caps.hpp"

#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"

#include <algorithm>
#include <cstdio>

namespace vitreo::winapi::d3d11 {

namespace {

#include "winapi/graphics/d3d11/d3d11_format_oracle.inc"

constexpr u32 S_OK_HR = 0x00000000u;
constexpr u32 E_FAIL_HR = 0x80004005u;
constexpr u32 E_INVALIDARG_HR = 0x80070057u;

/// The Windows answer for the device's feature level: the FL 11_0 masks for
/// 11_0 (the highest level this device is created at), and the FL-tagged
/// grids WARP printed for the lower levels. There is no 9_2 grid; 9_2 answers
/// as 9_1, whose format set it shares (Learn, FL 9.x format tables).
FormatSupportAnswer windows_support_at(u32 format, u32 feature_level) {
    if (format >= std::size(kWindowsFormatSupport)) return {E_FAIL_HR, 0};
    const auto& row = kWindowsFormatSupport[format];
    if (feature_level >= 0xB000u) return {row.hr, row.support};
    if (feature_level >= 0xA100u) return {row.hr_10_1, row.support_10_1};
    if (feature_level >= 0xA000u) return {row.hr_10_0, row.support_10_0};
    if (feature_level >= 0x9300u) return {row.hr_9_3, row.support_9_3};
    return {row.hr_9_1, row.support_9_1};
}

/// A device of d3d10.dll's D3D10CreateDevice (the Direct3D 10.0 API) sees
/// the DXGI 1.0 formats only: every format from B5G6R5_UNORM (85) on answers
/// E_FAIL, and the rest lose the D3D11-only bits above
/// D3D10_FORMAT_SUPPORT_SHADER_GATHER (BACK_BUFFER_CAST on R8G8B8A8 and
/// R10G10B10A2). Measured on Windows against a D3D10CreateDevice1 device
/// at 10_0, which answers exactly as a D3D11 device at 10_0 does.
constexpr u32 kFirstDxgi11Format = 85u;
constexpr u32 kD3D10FormatSupportMask = 0x00FFFFFFu;

FormatSupportAnswer windows_support(u32 format,
                                    const FormatCapsContext& context) {
    if (context.dxgi_1_0_formats_only && format >= kFirstDxgi11Format) {
        return {E_FAIL_HR, 0};
    }
    FormatSupportAnswer answer =
        windows_support_at(format, context.feature_level);
    if (context.dxgi_1_0_formats_only) answer.mask &= kD3D10FormatSupportMask;
    return answer;
}

bool is_float32_format(graphics::PixelFormat format) {
    return format == graphics::PixelFormat::R32Float ||
           format == graphics::PixelFormat::RG32Float ||
           format == graphics::PixelFormat::RGBA32Float;
}

/// Depth-plane view formats and the typeless depth families. Their host
/// storage is a depth texture whatever the bind flags.
bool is_depth_family(u32 format) {
    switch (format) {
    case 19: case 20: case 21: case 22:
    case 40:
    case 44: case 45: case 46: case 47:
    case 55:
        return true;
    default:
        return false;
    }
}

/// The formats a swap chain presents (area 11 owns the swap chain): the
/// bit-block-transfer set measured on Windows, less R10G10B10_XR_BIAS_A2,
/// which has no presentation path here.
bool is_display_format(u32 format) {
    switch (format) {
    case 10: case 24: case 28: case 29: case 87: case 91:
        return true;
    default:
        return false;
    }
}

bool any_sample_count(const FormatCapsContext& context) {
    return context.sample_count_2 || context.sample_count_4 ||
           context.sample_count_8 || context.sample_count_16;
}

/// What this implementation does with the packed and palettized DXGI video
/// formats (Learn DXGI_FORMAT; "Direct3D video resource formats"), or ~0u
/// for any other format. A shader reaches a packed format only through its
/// element views -- R8G8B8A8 (AYUV, YUY2), R10G10B10A2 (Y410), R16G16B16A16
/// (Y416, Y210, Y216), stored as exactly that format -- which load, sample,
/// gather and, at FL 11_0, take typed UAV stores; AYUV alone also renders
/// and blends ("Supported view types are SRV, RTV, and UAV"). The Windows row
/// decides mip chains (AYUV and Y416 carry MIP, AYUV MIP_AUTOGEN). 420_OPAQUE
/// is a Texture2D nothing reads ("You cannot use shaders with this format",
/// no CPU map); P8 and A8P8 are Texture2Ds the CPU maps and no shader reads
/// (measured, d3d11_video_formats_probe: SRV binds refused). No video
/// decoder, processor or encoder exists here, so none of those bits is
/// honoured.
u32 video_format_support(u32 format, const FormatCapsContext& context) {
    constexpr u32 element_view =
        FORMAT_SUPPORT_TEXTURE2D | FORMAT_SUPPORT_SHADER_LOAD |
        FORMAT_SUPPORT_SHADER_SAMPLE | FORMAT_SUPPORT_SHADER_GATHER |
        FORMAT_SUPPORT_MIP | FORMAT_SUPPORT_CPU_LOCKABLE;
    const u32 typed_uav = context.feature_level >= 0xB000u
        ? FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW : 0u;
    switch (format) {
    case 100:  // AYUV: RGBA8Unorm storage, rendered and mip-generated
        return element_view | typed_uav | FORMAT_SUPPORT_RENDER_TARGET |
               FORMAT_SUPPORT_BLENDABLE | FORMAT_SUPPORT_MIP_AUTOGEN;
    case 101:  // Y410
    case 102:  // Y416
    case 107:  // YUY2
    case 108:  // Y210
    case 109:  // Y216
        return element_view | typed_uav;
    case 106:  // 420_OPAQUE
        return FORMAT_SUPPORT_TEXTURE2D;
    case 113:  // P8
    case 114:  // A8P8
        return FORMAT_SUPPORT_TEXTURE2D | FORMAT_SUPPORT_CPU_LOCKABLE;
    default:
        return ~0u;
    }
}

/// Bits of the Windows mask this implementation honours for @p format.
u32 honoured_support(u32 format, const FormatCapsContext& context) {
    u32 honoured = 0;
    // A structured / raw buffer is DXGI_FORMAT_UNKNOWN; CPU access to one
    // is Map of a DYNAMIC or STAGING buffer.
    if (format == 0) {
        return FORMAT_SUPPORT_BUFFER | FORMAT_SUPPORT_CPU_LOCKABLE;
    }
    // BUFFER: a buffer resource can hold data of the format -- a typed
    // buffer view of it, or vertex data the input assembler fetches.
    if (typed_buffer_element_bytes(format) != 0) {
        honoured |= FORMAT_SUPPORT_BUFFER;
    }
    if (vertex_format_info(format) != nullptr) {
        honoured |= FORMAT_SUPPORT_BUFFER | FORMAT_SUPPORT_IA_VERTEX_BUFFER;
    }
    // IASetIndexBuffer takes R16_UINT and R32_UINT (Learn).
    if (format == 42 || format == 57) {
        honoured |= FORMAT_SUPPORT_IA_INDEX_BUFFER;
    }
    // Stream output writes the declared components into a formatless buffer
    // (d3d11_stream_out.cpp); the bit follows the Windows mask.
    honoured |= FORMAT_SUPPORT_SO_BUFFER;

    const graphics::PixelFormat storage = texture_storage_format(format);
    if (storage == graphics::PixelFormat::Unknown) return honoured;
    if (const u32 video = video_format_support(format, context);
        video != ~0u) {
        return honoured | video;
    }
    if (graphics::is_planar_format(storage)) {
        // Planar video (NV12 / P010 / P016): a one-mip Texture2D whose two
        // planes shaders load, sample, gather, render to and write through
        // plane views (Learn DXGI_FORMAT: SRV, RTV and UAV views), and that
        // the CPU maps. No video decoder, processor or encoder exists here,
        // so none of those bits is honoured.
        return honoured | FORMAT_SUPPORT_TEXTURE2D |
               FORMAT_SUPPORT_SHADER_LOAD | FORMAT_SUPPORT_SHADER_SAMPLE |
               FORMAT_SUPPORT_SHADER_GATHER | FORMAT_SUPPORT_RENDER_TARGET |
               FORMAT_SUPPORT_BLENDABLE | FORMAT_SUPPORT_CPU_LOCKABLE |
               FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    }
    const auto info = graphics::format_table(storage);
    honoured |= FORMAT_SUPPORT_TEXTURE1D | FORMAT_SUPPORT_TEXTURE2D |
                FORMAT_SUPPORT_TEXTURECUBE | FORMAT_SUPPORT_MIP |
                FORMAT_SUPPORT_CPU_LOCKABLE |
                FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT;
    // A volume binds to a shader without a view (no view swizzle), so the
    // formats that are read through one are not volumes here.
    if (view_swizzle_for(format, false) == ViewSwizzle::Identity) {
        honoured |= FORMAT_SUPPORT_TEXTURE3D;
    }
    // R10G10B10_XR_BIAS_A2_UNORM is creatable only as opaque 32-bit storage:
    // no shader, render-target or presentation path reads its encoding.
    if (format == 89) return honoured;

    const graphics::PixelFormat view = graphics::pixel_format_from_dxgi(format);
    const bool typeless = dxgi_format_is_typeless(format);
    const bool msaa = any_sample_count(context);
    if (is_depth_family(format)) {
        // The depth families were measured bit for bit against WARP
        // (d3d11_depth_format_support_probe) and are all backed by a Metal
        // depth texture, which loads, samples, compares and gathers.
        honoured |= FORMAT_SUPPORT_DEPTH_STENCIL |
                    FORMAT_SUPPORT_SHADER_LOAD | FORMAT_SUPPORT_SHADER_SAMPLE |
                    FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON |
                    FORMAT_SUPPORT_SHADER_GATHER |
                    FORMAT_SUPPORT_SHADER_GATHER_COMPARISON;
        if (msaa) {
            honoured |= FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET |
                        FORMAT_SUPPORT_MULTISAMPLE_LOAD;
        }
        return honoured;
    }
    if (typeless || view == graphics::PixelFormat::Unknown) {
        return honoured;
    }

    const bool integer = graphics::is_integer_format(view);
    const bool float32 = is_float32_format(view);
    const bool swizzled_storage = view == graphics::PixelFormat::ABGR4Unorm;
    honoured |= FORMAT_SUPPORT_SHADER_LOAD;
    // Metal filters every non-integer format except 32-bit float, which needs
    // supports32BitFloatFiltering. Gather reads four texels without
    // filtering.
    if (!integer && (!float32 || context.float32_filtering)) {
        honoured |= FORMAT_SUPPORT_SHADER_SAMPLE;
    }
    honoured |= FORMAT_SUPPORT_SHADER_GATHER;
    if (info.is_compressed) return honoured;

    // A render target of the storage format. B4G4R4A4 is stored with its
    // components permuted; a view swizzle fixes reads but not render-target
    // writes, so it is not renderable. Rendering to RGB9E5 is optional at
    // FL 11_0 (Learn table; D3D11.3 functional spec §19.3.2) and not
    // implemented here.
    const bool renderable =
        !swizzled_storage && view != graphics::PixelFormat::RGB9E5Float;
    if (renderable) {
        honoured |= FORMAT_SUPPORT_RENDER_TARGET;
        if (!integer) honoured |= FORMAT_SUPPORT_BLENDABLE;
        // GenerateMips is Metal's generateMipmaps, which needs a
        // colour-renderable, filterable format.
        if (!integer && (!float32 || context.float32_filtering)) {
            honoured |= FORMAT_SUPPORT_MIP_AUTOGEN;
        }
        if (msaa) {
            honoured |= FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET |
                        FORMAT_SUPPORT_MULTISAMPLE_LOAD;
            // Metal resolves no integer format, and 32-bit float only on
            // Apple9+ (Metal Feature Set Tables, "Resolve").
            if (!integer && (!float32 || context.float32_msaa_resolve)) {
                honoured |= FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
            }
        }
    }
    if (context.feature_level >= 0xB000u && supports_typed_uav_store(view)) {
        honoured |= FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    }
    if (is_display_format(format)) {
        honoured |= FORMAT_SUPPORT_DISPLAY;
    }
    // Swap-chain back buffers are created view-castable (create_texture_2d in
    // d3d11_texture_create.cpp: pixel_format_view for a swap-chain owner).
    if (is_display_format(format) || format == 91) {
        honoured |= FORMAT_SUPPORT_BACK_BUFFER_CAST;
    }
    return honoured;
}

u32 honoured_support2(u32 format, const FormatCapsContext& context) {
    const graphics::PixelFormat storage = texture_storage_format(format);
    if (storage == graphics::PixelFormat::Unknown) return 0;
    // FORMAT_SUPPORT2_TILED: a tiled 2D texture of the format has the
    // standard tile shape on this GPU (FormatCapsContext::tiled_format);
    // none below feature level 11_0 (measured WARP at 10_1: no format).
    u32 honoured = 0;
    if (context.tiled_resources && context.feature_level >= 0xB000u &&
        format < context.tiled_format.size() && context.tiled_format[format]) {
        honoured |= FORMAT_SUPPORT2_TILED;
    }
    // R10G10B10_XR_BIAS_A2 is storage only here: nothing but TILED.
    if (format == 89) return honoured;
    const graphics::PixelFormat view = graphics::pixel_format_from_dxgi(format);
    if (graphics::is_planar_format(storage) ||
        dxgi_format_has_video_views(format)) {
        // Typed UAV stores through a plane view (R8 / R8G8, R16 / R16G16)
        // or a packed format's element view; 420_OPAQUE has no view.
        if (context.feature_level >= 0xB000u && format != 106u) {
            honoured |= FORMAT_SUPPORT2_UAV_TYPED_STORE;
        }
    } else if (context.feature_level >= 0xB000u &&
        !dxgi_format_is_typeless(format)) {
        if (supports_typed_uav_load(view)) {
            honoured |= FORMAT_SUPPORT2_UAV_TYPED_LOAD;
        }
        if (supports_typed_uav_store(view)) {
            honoured |= FORMAT_SUPPORT2_UAV_TYPED_STORE;
        }
        // The translator lowers Interlocked* on a typed UAV to a Metal
        // texture atomic for 32-bit integer views only
        // (dxbc_to_spirv.cpp, ATOMIC: typed UAV).
        if (view == graphics::PixelFormat::R32Uint ||
            view == graphics::PixelFormat::R32Sint) {
            honoured |= FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
                        FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
                        FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
                        FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
                        FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
                        FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
        }
    }
    // D3D11.3 17.7: "Implementations that expose Logic Ops support must
    // support them for all renderable UINT formats" -- the formats whose
    // Windows row carries the bit (WARP: every renderable *_UINT).
    if (context.logic_ops &&
        (honoured_support(format, context) & FORMAT_SUPPORT_RENDER_TARGET) !=
            0) {
        honoured |= FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP;
    }
    // Learn D3D11_FORMAT_SUPPORT2_SHAREABLE: "Format supports shareable
    // resources". A shared texture of any shape is either an IOSurface image
    // or mirrored through a canonical store (metal_shared_resource.hpp),
    // which holds every format the backend can copy to memory: colour,
    // block-compressed, depth and stencil formats alike.
    if (static_cast<usize>(storage) < context.shared_storage.size() &&
        context.shared_storage[static_cast<usize>(storage)] &&
        (honoured_support(format, context) & FORMAT_SUPPORT_TEXTURE2D) != 0) {
        honoured |= FORMAT_SUPPORT2_SHAREABLE;
    }
    return honoured;
}

}  // namespace

bool dxgi_format_assigned(u32 format) noexcept {
    return format <= 115u || (format >= 130u && format <= 132u) ||
           (format >= 189u && format <= 191u);
}

graphics::PixelFormat texture_storage_format(u32 format) noexcept {
    // R32G32B32 has no Metal texel format; Vitreo does not emulate it for
    // textures (a typed-buffer view of it is a separate path).
    if (format >= 5 && format <= 8) return graphics::PixelFormat::Unknown;
    if (format == 89) return graphics::PixelFormat::R32Uint;
    return graphics::pixel_format_from_dxgi(format);
}

FormatSupportAnswer check_format_support(
    u32 format, const FormatCapsContext& context) noexcept {
    const FormatSupportAnswer windows =
        windows_support(format, context);
    if (windows.hr != S_OK_HR) return {windows.hr, 0};
    return {S_OK_HR, windows.mask & honoured_support(format, context)};
}

u32 unhonoured_format_support(u32 format,
                              const FormatCapsContext& context) noexcept {
    const FormatSupportAnswer windows =
        windows_support(format, context);
    if (windows.hr != S_OK_HR) return 0;
    return windows.mask & ~honoured_support(format, context);
}

u32 windows_support2(u32 format) noexcept {
    if (format >= std::size(kWindowsFormatSupport)) return 0;
    const auto& row = kWindowsFormatSupport[format];
    return row.hr2 == S_OK_HR ? row.support2 : 0u;
}

FormatSupportAnswer check_format_support2(
    u32 format, const FormatCapsContext& context) noexcept {
    if (format >= std::size(kWindowsFormatSupport)) return {E_FAIL_HR, 0};
    const auto& row = kWindowsFormatSupport[format];
    // A format the device cannot use at this feature level has no SUPPORT2
    // answer either.
    if (row.hr2 != S_OK_HR ||
        windows_support(format, context).hr != S_OK_HR) {
        return {E_FAIL_HR, 0};
    }
    const u32 mask = row.support2 & honoured_support2(format, context);
    return mask == 0 ? FormatSupportAnswer{E_FAIL_HR, 0}
                     : FormatSupportAnswer{S_OK_HR, mask};
}

u32 check_multisample_quality_levels(u32 format, u32 sample_count,
                                     const FormatCapsContext& context,
                                     u32& levels) noexcept {
    // Measured (d3d11_a01_device_caps_probe "msaa fmt"): the runtime rejects
    // the formats it has no entry for before looking at the count, then
    // answers E_FAIL with 0 for a count of 0 or above
    // D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT (32).
    if (format > 115u || format == 66u) return E_INVALIDARG_HR;
    if (sample_count == 0 || sample_count > 32u) {
        levels = 0;
        return E_FAIL_HR;
    }
    // One quality level (level 0) at x1 for every format the runtime knows
    // -- WARP answers 1 for UNKNOWN, BC, R32G32B32 and even NV11 / AI44 /
    // IA44, which CheckFormatSupport fails and no texture can be made of
    // (measured, d3d11_a01_device_caps_probe "fmt 110"; d3d11_video_formats_
    // probe [caps]) -- and at xN for a format that renders (or is depth)
    // multisampled at that count. A typeless format answers for its
    // family's renderable members.
    if (sample_count == 1) {
        levels = 1u;
        return S_OK_HR;
    }
    const bool creatable =
        format == 0 || texture_storage_format(format) !=
                           graphics::PixelFormat::Unknown;
    bool count_ok = false;
    switch (sample_count) {
    case 2: count_ok = context.sample_count_2; break;
    case 4: count_ok = context.sample_count_4; break;
    case 8: count_ok = context.sample_count_8; break;
    case 16: count_ok = context.sample_count_16; break;
    default: break;
    }
    const auto multisample_renderable = [&context](u32 member) {
        return (check_format_support(member, context).mask &
                FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) != 0;
    };
    bool renderable = false;
    if (creatable && count_ok) {
        const auto* family = dxgi_typeless_family(format);
        if (family != nullptr && family->typeless == format) {
            for (u8 i = 0; i < family->typed_count && !renderable; ++i) {
                renderable = multisample_renderable(family->typed[i]);
            }
        } else {
            renderable = multisample_renderable(format);
        }
    }
    levels = renderable ? 1u : 0u;
    return S_OK_HR;
}

bool supports_typed_uav_load(graphics::PixelFormat pixel_format) noexcept {
    // Learn (Typed unordered access view (UAV) loads): R32_FLOAT, R32_UINT and
    // R32_SINT always support a typed load and are required of D3D11.0
    // hardware. A further group is all-or-nothing and reported through
    // D3D11_FEATURE_DATA_D3D11_OPTIONS2::TypedUAVLoadAdditionalFormats. Load
    // support is the Metal Tier-2 read/write set.
    switch (pixel_format) {
    case graphics::PixelFormat::R32Float:
    case graphics::PixelFormat::R32Uint:
    case graphics::PixelFormat::R32Sint:
    case graphics::PixelFormat::RGBA32Float:
    case graphics::PixelFormat::RGBA32Uint:
    case graphics::PixelFormat::RGBA32Sint:
    case graphics::PixelFormat::RGBA16Float:
    case graphics::PixelFormat::RGBA16Uint:
    case graphics::PixelFormat::RGBA16Sint:
    case graphics::PixelFormat::RGBA8Unorm:
    case graphics::PixelFormat::RGBA8Uint:
    case graphics::PixelFormat::RGBA8Sint:
    case graphics::PixelFormat::R16Float:
    case graphics::PixelFormat::R16Uint:
    case graphics::PixelFormat::R16Sint:
    case graphics::PixelFormat::R8Unorm:
    case graphics::PixelFormat::R8Uint:
    case graphics::PixelFormat::R8Sint:
        return true;
    default:
        return false;
    }
}

bool supports_typed_uav_store(graphics::PixelFormat format) noexcept {
    // Metal Feature Set Tables, "Texture capabilities by pixel format",
    // Apple7+: these formats support Write although they cannot be bound
    // read_write. The packed 16-bit formats (B5G6R5, BGR5A1, ABGR4) and
    // RGB9E5 have no Write on any family.
    if (supports_typed_uav_load(format)) return true;
    switch (format) {
    case graphics::PixelFormat::A8Unorm:
    case graphics::PixelFormat::R8Snorm:
    case graphics::PixelFormat::R16Unorm:
    case graphics::PixelFormat::R16Snorm:
    case graphics::PixelFormat::RG8Unorm:
    case graphics::PixelFormat::RG8Snorm:
    case graphics::PixelFormat::RG8Uint:
    case graphics::PixelFormat::RG8Sint:
    case graphics::PixelFormat::RG16Unorm:
    case graphics::PixelFormat::RG16Snorm:
    case graphics::PixelFormat::RG16Float:
    case graphics::PixelFormat::RG16Uint:
    case graphics::PixelFormat::RG16Sint:
    case graphics::PixelFormat::RG32Float:
    case graphics::PixelFormat::RG32Uint:
    case graphics::PixelFormat::RG32Sint:
    case graphics::PixelFormat::RGBA8Snorm:
    case graphics::PixelFormat::RGBA16Unorm:
    case graphics::PixelFormat::RGBA16Snorm:
    case graphics::PixelFormat::RGB10A2Unorm:
    case graphics::PixelFormat::RGB10A2Uint:
    case graphics::PixelFormat::RG11B10Float:
    // BGRA8Unorm has Write on every Apple family; a typed-UAV store of
    // B8G8R8A8_UNORM matches Windows byte for byte (d3d11_a08_formats_probe
    // "F087 uavst").
    case graphics::PixelFormat::BGRA8Unorm:
        return true;
    default:
        return false;
    }
}

u32 typed_buffer_element_bytes(u32 dxgi_format) noexcept {
    // Learn (Buffer / RWBuffer): a typed buffer's element is one texel of the
    // view's format, expanded to four components. The view aliases the
    // buffer as a Metal texture buffer, so the element is the texel size.
    // Block-compressed and depth formats are not texel-addressable, and a
    // format that needs a view swizzle (B8G8R8X8, B4G4R4A4) or is packed
    // below 32 bits per texel (B5G6R5, B5G5R5A1) or shared-exponent
    // (R9G9B9E5) has no texture-buffer form.
    // The video formats (100-114) have no buffer form either: their rows
    // carry no BUFFER bit, and their storage format is a texture's alone.
    const auto pixel_format = graphics::pixel_format_from_dxgi(dxgi_format);
    if (pixel_format == graphics::PixelFormat::Unknown) return 0;
    if (dxgi_format >= 100 && dxgi_format <= 114) return 0;
    switch (dxgi_format) {
    case 67: case 85: case 86: case 88: case 92: case 93: case 115:
        return 0;
    default:
        break;
    }
    const auto info = graphics::format_table(pixel_format);
    if (info.is_compressed || info.is_depth) return 0;
    return info.bytes_per_pixel;
}

bool dxgi_format_has_alpha(u32 dxgi_format) noexcept {
    // Every DXGI_FORMAT whose name has an alpha field, taken from the
    // dxgiformat.h names in winapi/graphics/d3d12/d3d12_constants.hpp
    // (name matches `A[0-9]`; AYUV / Y410 / Y416, which also carry alpha,
    // are video formats with no shader view of this kind).
    switch (dxgi_format) {
    case 1: case 2: case 3: case 4:                // R32G32B32A32_*
    case 9: case 10: case 11: case 12: case 13: case 14:  // R16G16B16A16_*
    case 23: case 24: case 25:                     // R10G10B10A2_*
    case 27: case 28: case 29: case 30: case 31: case 32:  // R8G8B8A8_*
    case 65:                                       // A8_UNORM
    case 86:                                       // B5G5R5A1_UNORM
    case 87: case 90: case 91:                     // B8G8R8A8_*
    case 89:                                       // R10G10B10_XR_BIAS_A2
    case 112:                                      // IA44
    case 114:                                      // A8P8
    case 115:                                      // B4G4R4A4_UNORM
        return true;
    default:
        return false;
    }
}

ViewSwizzle view_swizzle_for(u32 view_format, bool depth_plane) noexcept {
    if (depth_plane) return ViewSwizzle::DepthRed;
    switch (view_format) {
    case 88: case 92: case 93:
        return ViewSwizzle::OpaqueAlpha;
    case 115:
        return ViewSwizzle::Bgra4;
    default:
        return ViewSwizzle::Identity;
    }
}

bool render_target_has_no_alpha(u32 rtv_format) noexcept {
    return rtv_format == 88 || rtv_format == 92 || rtv_format == 93;
}

namespace {

// d3d11.h enumerations the rules below name.
constexpr u32 USAGE_DEFAULT = 0;
constexpr u32 USAGE_IMMUTABLE = 1;
constexpr u32 USAGE_DYNAMIC = 2;
constexpr u32 USAGE_STAGING = 3;
constexpr u32 BIND_VERTEX_BUFFER = 0x1;
constexpr u32 BIND_INDEX_BUFFER = 0x2;
constexpr u32 BIND_CONSTANT_BUFFER = 0x4;
constexpr u32 BIND_SHADER_RESOURCE = 0x8;
constexpr u32 BIND_STREAM_OUTPUT = 0x10;
constexpr u32 BIND_RENDER_TARGET = 0x20;
constexpr u32 BIND_DEPTH_STENCIL = 0x40;
constexpr u32 BIND_UNORDERED_ACCESS = 0x80;
constexpr u32 BIND_DECODER = 0x200;
constexpr u32 BIND_VIDEO_ENCODER = 0x400;
constexpr u32 CPU_ACCESS_WRITE = 0x10000;
constexpr u32 CPU_ACCESS_READ = 0x20000;
constexpr u32 MISC_GENERATE_MIPS = 0x1;
constexpr u32 MISC_SHARED = 0x2;
constexpr u32 MISC_TEXTURECUBE = 0x4;
constexpr u32 MISC_DRAWINDIRECT_ARGS = 0x10;
constexpr u32 MISC_BUFFER_ALLOW_RAW_VIEWS = 0x20;
constexpr u32 MISC_BUFFER_STRUCTURED = 0x40;
constexpr u32 MISC_RESOURCE_CLAMP = 0x80;
constexpr u32 MISC_SHARED_KEYEDMUTEX = 0x100;
constexpr u32 MISC_GDI_COMPATIBLE = 0x200;
constexpr u32 MISC_SHARED_NTHANDLE = 0x800;
constexpr u32 MISC_GUARDED = 0x8000;
constexpr u32 MISC_RESTRICT_SHARED_RESOURCE = 0x2000;
constexpr u32 MISC_RESTRICT_SHARED_RESOURCE_DRIVER = 0x4000;
constexpr u32 MISC_TILE_POOL = 0x20000;
constexpr u32 MISC_TILED = 0x40000;
constexpr u32 STANDARD_MULTISAMPLE_PATTERN = 0xFFFFFFFFu;
constexpr u32 CENTER_MULTISAMPLE_PATTERN = 0xFFFFFFFEu;

bool is_typed_depth(u32 format) {
    return format == 20 || format == 40 || format == 45 || format == 55;
}

/// Typeless formats whose family has a depth member: the resource formats a
/// depth-stencil view can be made on besides the typed depth ones.
bool is_depth_capable_typeless(u32 format) {
    return format == 19 || format == 39 || format == 44 || format == 53;
}

/// The depth-plane / stencil-plane view formats and the typeless depth
/// families, which cannot be render targets or UAVs.
bool is_depth_only_family(u32 format) {
    return format == 19 || format == 21 || format == 22 ||
           format == 44 || format == 46 || format == 47;
}

bool is_block_compressed(u32 format) {
    return (format >= 70 && format <= 84) || (format >= 94 && format <= 99);
}

u32 family_support_or(u32 format, const FormatCapsContext& context,
                      bool whole_family) {
    const u32 own = check_format_support(format, context).mask;
    const auto* family = dxgi_typeless_family(format);
    if (family == nullptr ||
        (!whole_family && family->typeless != format)) {
        return own;
    }
    u32 mask = own;
    for (u8 i = 0; i < family->typed_count; ++i) {
        mask |= check_format_support(family->typed[i], context).mask;
    }
    return mask;
}

/// The Windows runtime's own shareable-format rule (FORMAT_SUPPORT2
/// SHAREABLE on the measured device), which it validates NTHANDLE against.
bool windows_shareable(u32 format) {
    return format < std::size(kWindowsFormatSupport) &&
           (kWindowsFormatSupport[format].support2 &
            FORMAT_SUPPORT2_SHAREABLE) != 0;
}

u32 full_chain(u32 width, u32 height, u32 depth) {
    u32 extent = std::max(width, std::max(height, depth));
    u32 levels = 1;
    while (extent > 1u) {
        extent >>= 1u;
        ++levels;
    }
    return levels;
}

}  // namespace

const char* texture_desc_violation(const TextureRuleDesc& desc,
                                   const FormatCapsContext& context) noexcept {
    const u32 format = desc.format;
    const FormatSupportAnswer support = check_format_support(format, context);
    const u32 dimension_bit =
        desc.dimension == 1 ? FORMAT_SUPPORT_TEXTURE1D
        : desc.dimension == 3 ? FORMAT_SUPPORT_TEXTURE3D
                              : FORMAT_SUPPORT_TEXTURE2D;
    if (support.hr != S_OK_HR || (support.mask & dimension_bit) == 0 ||
        texture_storage_format(format) == graphics::PixelFormat::Unknown) {
        return "format cannot be a texture of this dimension";
    }
    if (desc.usage > USAGE_STAGING) return "usage";
    const u32 bind = desc.bind_flags;
    const u32 misc = desc.misc_flags;
    const u32 cpu = desc.cpu_access_flags;
    // A texture binds only as a shader resource, render target, depth
    // stencil or UAV; buffer and video binds are E_INVALIDARG.
    // NV12 also takes VIDEO_ENCODER (measured, d3d11_nv12_probe [create]
    // "bind ENCODER"; Learn, Direct3D 11.4 "Extended NV12 texture support":
    // the video-encode flag "will be set all the time" for NV12). Nothing
    // here encodes; the flag only declares the use.
    const u32 extra_binds = format == 103u ? BIND_VIDEO_ENCODER : 0u;
    if ((bind & ~(BIND_SHADER_RESOURCE | BIND_RENDER_TARGET |
                  BIND_DEPTH_STENCIL | BIND_UNORDERED_ACCESS |
                  extra_binds)) != 0) {
        return "bind flags";
    }
    if ((cpu & ~(CPU_ACCESS_READ | CPU_ACCESS_WRITE)) != 0) {
        return "CPU access flags";
    }
    // GUARDED is accepted on every legal descriptor and changes nothing a
    // device without protected content exposes (measured: the "GU" column).
    // TILED, TILE_POOL, HW_PROTECTED and RESTRICTED_CONTENT need
    // capabilities this device reports absent (OPTIONS1/2 tiled tier 0).
    // RESTRICT_SHARED_RESOURCE(_DRIVER) restrict who opens a shared
    // resource: Learn, "only when you create a shared surface" (measured:
    // accepted with SH / KM, with and without NTHANDLE; d3d11_share_probe
    // [shapes]).
    const u32 restrict_flags =
        MISC_RESTRICT_SHARED_RESOURCE | MISC_RESTRICT_SHARED_RESOURCE_DRIVER;
    if ((misc & ~(MISC_GENERATE_MIPS | MISC_SHARED | MISC_TEXTURECUBE |
                  MISC_RESOURCE_CLAMP | MISC_SHARED_KEYEDMUTEX |
                  MISC_GDI_COMPATIBLE | MISC_SHARED_NTHANDLE |
                  MISC_GUARDED | MISC_TILED | restrict_flags)) != 0 ||
        ((misc & restrict_flags) != 0 &&
         (misc & (MISC_SHARED | MISC_SHARED_KEYEDMUTEX)) == 0)) {
        return "misc flags";
    }

    // Dimension limits: D3D11_REQ_TEXTURE1D_U_DIMENSION /
    // TEXTURE2D_U_OR_V_DIMENSION 16384, TEXTURE3D_U_V_OR_W_DIMENSION 2048,
    // TEXTURE1D/2D_ARRAY_AXIS_DIMENSION 2048.
    // At 10_0 and 10_1 an array holds at most 512 slices
    // (D3D10_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION; measured,
    // d3d10_contract_probe: 512 S_OK, 513 E_INVALIDARG -- while the U / V
    // extent is not held to 10_x's 8192, 8193 succeeding).
    const u32 extent_limit = desc.dimension == 3 ? 2048u : 16384u;
    const u32 array_limit = context.feature_level >= 0xA000u &&
                                    context.feature_level < 0xB000u
                                ? 512u
                                : 2048u;
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0 ||
        desc.width > extent_limit || desc.height > extent_limit ||
        desc.depth > extent_limit || desc.array_size == 0 ||
        desc.array_size > array_limit) {
        return "dimensions";
    }
    // MipLevels above the full chain is E_INVALIDARG (measured: "Mips=8
    // W=64", "Mips=2 1x1"); Metal aborts the process on such a descriptor.
    const u32 chain = full_chain(desc.width, desc.height, desc.depth);
    const u32 mips = desc.mip_levels == 0 ? chain : desc.mip_levels;
    if (mips > chain) return "MipLevels beyond the full chain";

    // Tiled resources (Learn, "Tiled resource creation parameters" and
    // "Tier 1"; measured, d3d11_tiled_probe [create]): a DEFAULT Texture2D
    // (Texture1D and Texture3D refused at Tier 1) without initial data, of a
    // FORMAT_SUPPORT2_TILED format, taking of the misc flags only
    // TEXTURECUBE, GENERATE_MIPS and RESOURCE_CLAMP (GUARDED, the sharing
    // flags and TILE_POOL refused), multisampled only at the counts
    // CheckMultisampleQualityLevels1(TILED) has (4x here, as on WARP);
    // an array only when every mip is a whole number of standard tiles in
    // both dimensions ("Tiled resources with any mipmaps not a multiple of
    // standard tile size in all dimensions are not allowed to have an array
    // size larger than 1"; measured: 256 a2 m2 S_OK, m3 and 200 a2
    // E_INVALIDARG, a 6-face cube of one 256 mip S_OK, the full chain not).
    if ((misc & MISC_TILED) != 0) {
        if (!context.tiled_resources || context.feature_level < 0xB000u ||
            desc.dimension != 2 || desc.usage != USAGE_DEFAULT ||
            desc.initial_data ||
            (misc & ~(MISC_TILED | MISC_TEXTURECUBE | MISC_GENERATE_MIPS |
                      MISC_RESOURCE_CLAMP)) != 0 ||
            format >= context.tiled_format.size() ||
            !context.tiled_format[format]) {
            return "tiled texture";
        }
        if (desc.sample_count > 1 &&
            (desc.sample_count != 4 || !context.tiled_format_4x[format])) {
            return "tiled texture sample count";
        }
        if (desc.array_size > 1) {
            u32 tile_w = 0;
            u32 tile_h = 0;
            if (!standard_tile_shape(format, desc.sample_count, tile_w,
                                     tile_h)) {
                return "tiled texture";
            }
            for (u32 mip = 0; mip < mips; ++mip) {
                if (std::max(1u, desc.width >> mip) % tile_w != 0 ||
                    std::max(1u, desc.height >> mip) % tile_h != 0) {
                    return "tiled array with a mip that is not whole tiles";
                }
            }
        }
    }

    const bool dynamic = desc.usage == USAGE_DYNAMIC;
    const bool immutable = desc.usage == USAGE_IMMUTABLE;
    const bool multisampled = desc.sample_count > 1;
    // Planar video (NV12 / P010 / P016): Learn DXGI_FORMAT, "Width and
    // height must be even"; measured (d3d11_nv12_probe [create]): one mip
    // (MipLevels 0 is the full chain, refused past 1), at most two array
    // slices, never multisampled, a cube or a depth target. GENERATE_MIPS
    // is accepted (with its one mip there is nothing to generate).
    const bool planar = graphics::is_planar_format(texture_storage_format(format));
    if (planar &&
        (desc.dimension != 2 || desc.width % 2u != 0 ||
         desc.height % 2u != 0 || mips != 1 || desc.array_size > 2 ||
         multisampled || (misc & MISC_TEXTURECUBE) != 0 ||
         (bind & BIND_DEPTH_STENCIL) != 0)) {
        return "planar video shape";
    }
    // The packed and palettized video formats, measured on Windows
    // (d3d11_video_formats_probe [create]): a packed 4:2:2 one has an even
    // width (Learn DXGI_FORMAT YUY2 / Y210 / Y216: "Width must be even");
    // one whose row has no FORMAT_SUPPORT_MIP (Y410, YUY2, Y210, Y216, P8,
    // A8P8) is a single mip, MipLevels 0 included; a shader-resource bind
    // needs a format a shader reads (P8, A8P8 and 420_OPAQUE refuse it); and
    // 420_OPAQUE -- "Applications cannot use the CPU to map the resource",
    // its layout "completely opaque" -- is DEFAULT only, without initial
    // data (STAGING, DYNAMIC and initial data all E_INVALIDARG).
    if (video_format_support(format, context) != ~0u) {
        if (dxgi_pixels_per_texel(format) != 1u &&
            desc.width % dxgi_pixels_per_texel(format) != 0) {
            return "packed 4:2:2 video width";
        }
        if ((support.mask & FORMAT_SUPPORT_MIP) == 0 && mips != 1) {
            return "video format without mip levels";
        }
        if ((bind & BIND_SHADER_RESOURCE) != 0 &&
            (support.mask & (FORMAT_SUPPORT_SHADER_LOAD |
                             FORMAT_SUPPORT_SHADER_SAMPLE)) == 0) {
            return "shader-resource bind on a format no shader reads";
        }
        if (format == 106u &&
            (desc.usage != USAGE_DEFAULT || desc.initial_data)) {
            return "420_OPAQUE with CPU access";
        }
    }
    const u32 sharing_flags =
        MISC_SHARED | MISC_SHARED_KEYEDMUTEX | MISC_SHARED_NTHANDLE;

    // DXGI_SAMPLE_DESC: Count >= 1; Quality below the level count, or one of
    // the standard / center patterns where the count is supported.
    if (desc.sample_count == 0) return "sample count 0";
    u32 levels = 0;
    (void)check_multisample_quality_levels(format, desc.sample_count, context,
                                           levels);
    if (desc.sample_quality == STANDARD_MULTISAMPLE_PATTERN ||
        desc.sample_quality == CENTER_MULTISAMPLE_PATTERN) {
        if (levels == 0) return "sample pattern at an unsupported count";
        // Metal's default sample positions for 2, 4 and 8 samples are the
        // D3D standard patterns; one sample sits at the pixel centre either
        // way. Moving every sample to the centre needs programmable sample
        // positions this backend does not set.
        if (desc.sample_quality == CENTER_MULTISAMPLE_PATTERN &&
            multisampled) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED d3d11!CreateTexture2D: "
                    "D3D11_CENTER_MULTISAMPLE_PATTERN\n");
            }
            return "unimplemented: D3D11_CENTER_MULTISAMPLE_PATTERN";
        }
    } else if (desc.sample_quality >= levels) {
        return "sample count / quality";
    }
    if (multisampled) {
        // Measured (d3d11_msaa_depth_probe, contracts "C S=4 ..."): one mip,
        // DEFAULT, no CPU access or initial data, bound as a render target,
        // depth target or shader resource -- never a UAV, never unbound.
        if (desc.dimension != 2 || mips != 1 ||
            desc.usage != USAGE_DEFAULT || cpu != 0 || desc.initial_data ||
            bind == 0 || (bind & BIND_UNORDERED_ACCESS) != 0 ||
            (misc & ~(MISC_RESOURCE_CLAMP | MISC_GUARDED | MISC_TILED |
                      sharing_flags)) != 0) {
            return "multisampled texture shape";
        }
    }

    // Formats: block compression (D3D11.3 functional spec §19.5; measured
    // "C BC1 5x5", "C BC1 8x8 RT/UAV", "C BC1 8x8 DYN").
    const bool block_compressed = is_block_compressed(format);
    if (block_compressed &&
        (desc.width % 4u != 0 || desc.height % 4u != 0 ||
         (bind & ~BIND_SHADER_RESOURCE) != 0 ||
         (misc & MISC_GENERATE_MIPS) != 0)) {
        return "block-compressed shape";
    }
    // Depth: a depth-stencil view needs a typed depth format or a typeless
    // one with a depth member; typed depth binds only as a depth target;
    // depth and UAV / render target never combine (measured).
    if ((bind & BIND_DEPTH_STENCIL) != 0 &&
        (desc.dimension == 3 ||
         (!is_typed_depth(format) && !is_depth_capable_typeless(format)) ||
         (bind & (BIND_RENDER_TARGET | BIND_UNORDERED_ACCESS)) != 0)) {
        return "depth-stencil bind";
    }
    if (is_typed_depth(format) && (bind & ~BIND_DEPTH_STENCIL) != 0) {
        return "typed depth format bound as other than depth stencil";
    }
    if (is_depth_only_family(format) &&
        (bind & (BIND_RENDER_TARGET | BIND_UNORDERED_ACCESS)) != 0) {
        return "depth family bound as a render target or UAV";
    }
    // A render target needs a renderable format (a typeless one through a
    // renderable member); a UAV a typed-UAV one. A Texture2D accepts a UAV
    // bind when any member of the format's family has typed UAVs (measured:
    // R8G8B8A8_UNORM_SRGB and B8G8R8A8_UNORM_SRGB 2D-UAV OK, 3D-UAV not).
    if ((bind & BIND_RENDER_TARGET) != 0 &&
        (family_support_or(format, context, false) &
         FORMAT_SUPPORT_RENDER_TARGET) == 0) {
        // Unimplemented only where a Windows device renders to it; a format
        // Windows cannot render to either (Y410, YUY2, BC, ...) is refused
        // as it is there.
        if ((unhonoured_format_support(format, context) &
             FORMAT_SUPPORT_RENDER_TARGET) != 0) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED d3d11!CreateTexture: render "
                    "target of DXGI format %u\n", format);
            }
        }
        return "render-target bind on a non-renderable format";
    }
    // D3D11.3 functional spec §5.3.9.5: a 32-bit-per-texel TYPELESS resource
    // takes an R32_UINT / R32_SINT / R32_FLOAT UAV whatever its family
    // (measured: B8G8R8X8_TYPELESS with a UAV bind, "C092 r32uav tex").
    const bool r32_castable =
        dxgi_format_is_typeless(format) && !is_depth_only_family(format) &&
        !is_block_compressed(format) &&
        graphics::format_table(texture_storage_format(format))
                .bytes_per_pixel == 4;
    if ((bind & BIND_UNORDERED_ACCESS) != 0 && !r32_castable &&
        (family_support_or(format, context, desc.dimension == 2) &
         FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) == 0) {
        return "UAV bind on a format without typed UAVs";
    }

    // Misc flags.
    const bool shared = (misc & MISC_SHARED) != 0;
    const bool keyed = (misc & MISC_SHARED_KEYEDMUTEX) != 0;
    const bool nt_handle = (misc & MISC_SHARED_NTHANDLE) != 0;
    if ((misc & MISC_GENERATE_MIPS) != 0 &&
        ((bind & (BIND_SHADER_RESOURCE | BIND_RENDER_TARGET)) !=
             (BIND_SHADER_RESOURCE | BIND_RENDER_TARGET) ||
         desc.usage != USAGE_DEFAULT || multisampled ||
         ((support.mask & FORMAT_SUPPORT_MIP) == 0 && !planar))) {
        // Learn D3D11_RESOURCE_MISC_GENERATE_MIPS: render target and shader
        // resource. Creation does not look at MIP_AUTOGEN (measured: GM on
        // R32_UINT succeeds).
        return "GENERATE_MIPS";
    }
    if ((misc & MISC_TEXTURECUBE) != 0 &&
        (desc.dimension != 2 || desc.width != desc.height ||
         desc.array_size % 6u != 0 || multisampled ||
         (support.mask & FORMAT_SUPPORT_TEXTURECUBE) == 0)) {
        return "TEXTURECUBE";
    }
    // Cube arrays are a 10_1 feature (Learn, "Direct3D feature levels":
    // TextureCubeArray from 10_1): below it a cube has exactly six faces
    // (measured, d3d10_contract_probe: a 12-face cube on a 10_0 device is
    // E_INVALIDARG, on 10_1 S_OK).
    if ((misc & MISC_TEXTURECUBE) != 0 && desc.array_size > 6u &&
        context.feature_level < 0xA100u) {
        return "cube array below feature level 10_1";
    }
    if ((misc & MISC_RESOURCE_CLAMP) != 0 &&
        desc.usage != USAGE_DEFAULT && !immutable) {
        return "RESOURCE_CLAMP";
    }
    if ((misc & MISC_GDI_COMPATIBLE) != 0 &&
        (desc.dimension != 2 ||
         (format != 87 && format != 90 && format != 91) ||
         (bind & BIND_RENDER_TARGET) == 0 || mips != 1 || multisampled ||
         desc.usage != USAGE_DEFAULT ||
         (misc & MISC_RESOURCE_CLAMP) != 0)) {
        // Learn D3D11_RESOURCE_MISC_GDI_COMPATIBLE: B8G8R8A8 formats, a
        // render target, one mip, no RESOURCE_CLAMP.
        return "GDI_COMPATIBLE";
    }
    if (shared || keyed || nt_handle) {
        // Measured on Windows (d3d11_a02_create_matrix_probe SH / KM /
        // SHNT / KMNT columns, contracts "C SH|..."): SHARED and KEYEDMUTEX
        // are exclusive and refused on a DYNAMIC resource; without NTHANDLE
        // the runtime validates nothing more (Learn D3D11_RESOURCE_MISC_FLAG:
        // "the runtime does not strictly validate shared resource
        // parameters"). NTHANDLE needs one of the two and a DEFAULT
        // Texture2D (TEXTURECUBE, MSAA and RESOURCE_CLAMP refused, measured
        // d3d11_share_probe [shapes] SHNT), and "the runtime then must confirm
        // that the shared resource works": a format this device shares
        // (FORMAT_SUPPORT2 SHAREABLE -- WARP refuses D24S8 there) in a shape
        // it shares. With ExtendedResourceSharing that takes mipmapped and
        // array Texture2Ds (Learn, "Extended support for shared Texture2D
        // resources"; measured d3d11_share_ers_probe "mips5 SHNT", "mips2
        // array3 KMNT"); without it the D3D11.1 guaranteed single image.
        // Without NTHANDLE whether a handle can be made is the sharing path's
        // business.
        const bool shareable_format =
            (check_format_support2(format, context).mask &
             FORMAT_SUPPORT2_SHAREABLE) != 0;
        // A shared resource never takes RESOURCE_CLAMP (measured: SH and
        // SH|NT both E_INVALIDARG; Learn lists RESOURCE_CLAMP among what
        // cannot be shared even with ExtendedResourceSharing).
        if ((shared && keyed) || (nt_handle && !shared && !keyed) ||
            dynamic || (misc & MISC_RESOURCE_CLAMP) != 0 ||
            (nt_handle && (desc.dimension != 2 ||
                           desc.usage != USAGE_DEFAULT ||
                           !windows_shareable(format) || !shareable_format ||
                           (!context.extended_sharing &&
                            (mips != 1 || desc.array_size != 1)) ||
                           multisampled ||
                           (misc & MISC_TEXTURECUBE) != 0))) {
            return "sharing flags";
        }
        // Learn D3D11_FORMAT_SUPPORT2_SHAREABLE: R8G8B8A8_UNORM(_SRGB)
        // "are never shareable when using feature level 9 ... Attempting to
        // create shared resources with [them] will always fail unless the
        // feature level is 10_0 or higher" (measured at 9_3: SH, KM and
        // SH|NT all E_INVALIDARG, [fl9_3]).
        if (context.feature_level < 0xA000u && (format == 28 || format == 29)) {
            return "sharing flags below feature level 10_0";
        }
    }

    // Usage (Learn D3D11_USAGE, footnotes; measured per usage).
    if (desc.usage == USAGE_DEFAULT) {
        // D3D11_OPTIONS2.MapOnDefaultTextures (D3D11.3): a DEFAULT texture
        // may take CPU access, measured on Windows
        // (d3d11_texmap_probe [create]): any bind but DEPTH_STENCIL and any
        // misc flag but RESOURCE_CLAMP (multisampled textures take no CPU
        // access at all, above); a 2D or 3D one never has a 12-byte
        // element (R32G32B32: no standard swizzle exists for it, under any
        // layout -- a Texture1D may); a volume other than
        // 64K_STANDARD_SWIZZLE is one mip; ROW_MAJOR is one mip, one array
        // slice and no bind. Without CPU access only UNDEFINED is a layout
        // (every other value E_INVALIDARG, [create] "u ... c0").
        const u32 layout = desc.layout;
        if (cpu == 0) {
            if (layout != 0) return "texture layout without CPU access";
        } else {
            constexpr u32 ROW_MAJOR = 1;
            constexpr u32 STANDARD_SWIZZLE_64K = 2;
            // dxgiformat.h: R32G32B32_TYPELESS 5, _FLOAT 6, _UINT 7, _SINT 8.
            const bool twelve_byte_element = format >= 5u && format <= 8u;
            if ((bind & BIND_DEPTH_STENCIL) != 0 ||
                (misc & MISC_RESOURCE_CLAMP) != 0) {
                return "DEFAULT with CPU access: depth-stencil or clamp";
            }
            if (desc.dimension != 1 && twelve_byte_element) {
                return "DEFAULT with CPU access: 96-bit format";
            }
            if (desc.dimension == 3 && layout != STANDARD_SWIZZLE_64K &&
                mips != 1) {
                return "DEFAULT volume with CPU access: mip levels";
            }
            if (layout == ROW_MAJOR &&
                (bind != 0 || mips != 1 || desc.array_size != 1)) {
                return "ROW_MAJOR shape";
            }
        }
    } else if (immutable) {
        if (cpu != 0 || !desc.initial_data || bind != BIND_SHADER_RESOURCE ||
            (misc & ~(MISC_TEXTURECUBE | MISC_RESOURCE_CLAMP |
                      MISC_GUARDED | sharing_flags)) != 0) {
            return "IMMUTABLE";
        }
    } else if (dynamic) {
        // "A dynamic resource can only be created with a single
        // subresource" (measured: mips 2, mips 0, arrays refused).
        if (cpu != CPU_ACCESS_WRITE || bind != BIND_SHADER_RESOURCE ||
            mips != 1 || desc.array_size != 1 ||
            (misc & ~MISC_GUARDED) != 0) {
            return "DYNAMIC";
        }
    } else {
        // STAGING: no pipeline binds, some CPU access, and of the misc flags
        // only TEXTURECUBE (DirectXTex CaptureTexture keeps it; measured).
        if (bind != 0 || cpu == 0 ||
            (misc & ~(MISC_TEXTURECUBE | MISC_GUARDED | sharing_flags)) != 0) {
            return "STAGING";
        }
    }
    return nullptr;
}

u32 buffer_desc_check(const BufferRuleDesc& desc,
                      const char** violation) noexcept {
    const auto fail = [violation](const char* why) {
        if (violation != nullptr) *violation = why;
        return E_INVALIDARG_HR;
    };
    if (violation != nullptr) *violation = nullptr;
    const u32 bind = desc.bind_flags;
    const u32 misc = desc.misc_flags;
    const u32 cpu = desc.cpu_access_flags;
    // Measured: the runtime ignores bind bit 0x100, DECODER and
    // VIDEO_ENCODER on a buffer (they count as "a bind" for the usages that
    // need one); a depth-stencil or any undefined bit is E_INVALIDARG.
    constexpr u32 ignored_binds = 0x100u | BIND_DECODER | BIND_VIDEO_ENCODER;
    constexpr u32 buffer_binds =
        BIND_VERTEX_BUFFER | BIND_INDEX_BUFFER | BIND_CONSTANT_BUFFER |
        BIND_SHADER_RESOURCE | BIND_STREAM_OUTPUT | BIND_RENDER_TARGET |
        BIND_UNORDERED_ACCESS;
    // A tile pool (Learn, "Tile pool creation"; measured, d3d11_tiled_probe
    // [create]): TILE_POOL alone, DEFAULT, nothing bound, no CPU access or
    // initial data, a whole number of 64 KiB tiles -- none included (0 S_OK).
    // Its tiles are memory allocated as they are mapped, so its size is not
    // the resource-size limit's (measured x64: 2 GiB and 0xFFFF0000 S_OK).
    if ((misc & MISC_TILE_POOL) != 0) {
        if (!desc.tiled_resources) return fail("TILE_POOL without tiled resources");
        if (misc != MISC_TILE_POOL || desc.usage != USAGE_DEFAULT ||
            bind != 0 || cpu != 0 || desc.initial_data ||
            desc.byte_width % 65536u != 0) {
            return fail("TILE_POOL");
        }
        return S_OK_HR;
    }
    // A tiled buffer (Learn, "Tiled resource creation parameters"; measured):
    // DEFAULT, no CPU access or initial data, bound (if at all) only as a
    // shader resource, UAV or render target, with the buffer-shape flags but
    // not RESOURCE_CLAMP, GUARDED or sharing; any ByteWidth, since nothing is
    // allocated (measured: 100 bytes and 0xFFFF0000 S_OK).
    const bool tiled = (misc & MISC_TILED) != 0;
    if (tiled) {
        if (!desc.tiled_resources) return fail("TILED without tiled resources");
        if (desc.usage != USAGE_DEFAULT || cpu != 0 || desc.initial_data ||
            (bind & ~(BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS |
                      BIND_RENDER_TARGET)) != 0 ||
            (misc & ~(MISC_TILED | MISC_DRAWINDIRECT_ARGS |
                      MISC_BUFFER_ALLOW_RAW_VIEWS |
                      MISC_BUFFER_STRUCTURED)) != 0) {
            return fail("TILED buffer");
        }
    }
    if (desc.byte_width == 0) return fail("ByteWidth 0");
    if (desc.usage > USAGE_STAGING) return fail("usage");
    if ((bind & ~(buffer_binds | ignored_binds)) != 0) return fail("bind");
    if ((cpu & ~(CPU_ACCESS_READ | CPU_ACCESS_WRITE)) != 0) {
        return fail("CPU access flags");
    }
    // Of the misc flags a buffer takes the three shape flags, GUARDED
    // (accepted unconditionally), and SHARED or KEYEDMUTEX without NTHANDLE,
    // which the runtime does not validate on a buffer except on a DYNAMIC or
    // CPU-accessible DEFAULT one (measured SH / KM columns; no handle can be
    // made for a buffer here). NTHANDLE is Texture2D-only.
    if ((misc & ~(MISC_DRAWINDIRECT_ARGS | MISC_BUFFER_ALLOW_RAW_VIEWS |
                  MISC_BUFFER_STRUCTURED | MISC_GUARDED | MISC_SHARED |
                  MISC_SHARED_KEYEDMUTEX | MISC_TILED)) != 0) {
        return fail("misc flags");
    }
    if ((misc & (MISC_SHARED | MISC_SHARED_KEYEDMUTEX)) != 0 &&
        ((misc & MISC_SHARED) != 0) == ((misc & MISC_SHARED_KEYEDMUTEX) != 0)) {
        return fail("SHARED with KEYEDMUTEX");
    }
    if ((misc & (MISC_SHARED | MISC_SHARED_KEYEDMUTEX)) != 0 &&
        (desc.usage == USAGE_DYNAMIC ||
         (desc.usage == USAGE_DEFAULT && cpu != 0))) {
        return fail("shared CPU-writable buffer");
    }
    const bool constant = (bind & BIND_CONSTANT_BUFFER) != 0;
    const bool gpu_output =
        (bind & (BIND_STREAM_OUTPUT | BIND_RENDER_TARGET |
                 BIND_UNORDERED_ACCESS)) != 0;
    // Learn D3D11_BIND_FLAG: a constant buffer binds alone. Its ByteWidth is a
    // multiple of 16; the 11.1 runtime creates constant buffers larger than
    // D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT constants (Learn CreateBuffer
    // remarks, measured "A CB 65552/1048576").
    if (constant && ((bind & ~(BIND_CONSTANT_BUFFER | ignored_binds)) != 0 ||
                     desc.byte_width % 16u != 0)) {
        return fail("constant buffer");
    }
    // Stream output never combines with a UAV (measured "A SO|UAV").
    if ((bind & BIND_STREAM_OUTPUT) != 0 &&
        (bind & BIND_UNORDERED_ACCESS) != 0) {
        return fail("stream output with UAV");
    }
    const bool structured = (misc & MISC_BUFFER_STRUCTURED) != 0;
    const bool raw = (misc & MISC_BUFFER_ALLOW_RAW_VIEWS) != 0;
    const bool indirect = (misc & MISC_DRAWINDIRECT_ARGS) != 0;
    if (structured) {
        // Measured: a non-zero multiple of 4 up to 2048 that tiles ByteWidth;
        // never a vertex, index, constant or stream-output buffer, never raw
        // or an argument buffer.
        const u32 stride = desc.structure_byte_stride;
        if (stride == 0 || stride % 4u != 0 || stride > 2048u ||
            desc.byte_width % stride != 0 || raw || indirect ||
            (bind & (BIND_VERTEX_BUFFER | BIND_INDEX_BUFFER |
                     BIND_CONSTANT_BUFFER | BIND_STREAM_OUTPUT |
                     BIND_RENDER_TARGET)) != 0) {
            return fail("BUFFER_STRUCTURED");
        }
    }
    // A raw buffer exists to be viewed (SRV or UAV) and is neither a
    // constant nor a stream-output buffer; ByteWidth has no alignment rule
    // at creation (measured "A RAW BW6").
    if (raw && ((bind & (BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS)) == 0 ||
                (bind & (BIND_CONSTANT_BUFFER | BIND_STREAM_OUTPUT)) != 0)) {
        return fail("BUFFER_ALLOW_RAW_VIEWS");
    }
    // An argument buffer holds at least one D3D11_DRAW_INSTANCED... record's
    // worth of arguments (measured: ByteWidth 12 is the minimum), is not a
    // constant buffer and is not staging memory.
    if (indirect && (desc.byte_width < 12u || constant ||
                     desc.usage == USAGE_STAGING)) {
        return fail("DRAWINDIRECT_ARGS");
    }
    switch (desc.usage) {
    case USAGE_DEFAULT:
        // MapOnDefaultBuffers (Learn D3D11_FEATURE_DATA_D3D11_OPTIONS1,
        // "Mappable default buffers": only the SHADER_RESOURCE and
        // UNORDERED_ACCESS binds). Measured (d3d11_capsmall_probe "mapdef"):
        // at least one of the two and nothing else -- not even the binds
        // other usages ignore -- and of the misc flags only
        // BUFFER_ALLOW_RAW_VIEWS and BUFFER_STRUCTURED; below feature level
        // 11_0 every CPU access flag is refused.
        if (cpu != 0 &&
            (!desc.map_on_default_buffers || bind == 0 ||
             (bind & ~(BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS)) != 0 ||
             (misc & ~(MISC_BUFFER_ALLOW_RAW_VIEWS |
                       MISC_BUFFER_STRUCTURED)) != 0)) {
            return fail("DEFAULT with CPU access");
        }
        break;
    case USAGE_IMMUTABLE:
        if (cpu != 0 || bind == 0 || gpu_output || !desc.initial_data) {
            return fail("IMMUTABLE");
        }
        break;
    case USAGE_DYNAMIC:
        if (cpu != CPU_ACCESS_WRITE || bind == 0 || gpu_output) {
            return fail("DYNAMIC");
        }
        break;
    default:
        // Measured: a staging buffer ignores the DECODER bind as other
        // usages do, but not 0x100 or any real pipeline bind.
        if ((bind & ~BIND_DECODER) != 0 || cpu == 0) return fail("STAGING");
        break;
    }
    // D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM: no resource
    // exceeds 2048 MiB; a larger request is E_OUTOFMEMORY (measured:
    // "A BW 2GiB" S_OK, "A BW 0xFFFFFFF0" 0x8007000E on x64).
    if (desc.byte_width > 0x80000000u && !tiled) {
        if (violation != nullptr) *violation = "larger than 2 GiB";
        return 0x8007000Eu;
    }
    return S_OK_HR;
}

bool standard_tile_shape(u32 dxgi_format, u32 samples, u32& width,
                         u32& height) noexcept {
    width = 0;
    height = 0;
    const graphics::PixelFormat storage = texture_storage_format(dxgi_format);
    if (storage == graphics::PixelFormat::Unknown ||
        graphics::is_planar_format(storage)) {
        return false;
    }
    if (is_block_compressed(dxgi_format)) {
        // 64-bit blocks (BC1, BC4): 128x64 blocks; 128-bit: 64x64 blocks.
        if (samples != 1) return false;
        const u32 block = graphics::compressed_block_bytes(storage);
        if (block == 8) {
            width = 512;
            height = 256;
        } else if (block == 16) {
            width = 256;
            height = 256;
        } else {
            return false;
        }
        return true;
    }
    // Element size -> tile, single sample and 4x (the spec's table: the
    // 64 KiB holds 65536 / bytes elements, square where that is a square
    // number, twice as wide as tall otherwise; 4x quarters the pixels).
    struct Shape {
        u32 bytes;
        u32 w1, h1;
        u32 w4, h4;
    };
    static constexpr Shape kShapes[] = {
        {1, 256, 256, 128, 128},
        {2, 256, 128, 128, 64},
        {4, 128, 128, 64, 64},
        {8, 128, 64, 64, 32},
        {16, 64, 64, 32, 32},
    };
    const u32 bytes = graphics::format_table(storage).bytes_per_pixel;
    for (const Shape& shape : kShapes) {
        if (shape.bytes != bytes) continue;
        if (samples == 1) {
            width = shape.w1;
            height = shape.h1;
            return true;
        }
        if (samples == 4) {
            width = shape.w4;
            height = shape.h4;
            return true;
        }
        return false;
    }
    return false;
}

}  // namespace vitreo::winapi::d3d11
