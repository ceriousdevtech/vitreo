// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_defaults.cpp
/// @brief Documented initial values of D3D9 device state, and D3DFORMAT
///        translation into the shared graphics core.
///
/// A guest that calls `GetRenderState` before ever calling `SetRenderState`
/// must read the value Windows starts the device with. Returning zero for
/// everything would, for example, report `D3DRS_CULLMODE` as an invalid enum
/// and `D3DRS_COLORWRITEENABLE` as "write no channels", so these tables are
/// part of the contract rather than a convenience.

#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "graphics/contracts/format_converter.hpp"
#include <cstring>
#include <vector>

namespace vitreo::winapi {
namespace {

// D3DRENDERSTATETYPE
constexpr u32 D3DRS_ZENABLE = 7;
constexpr u32 D3DRS_FILLMODE = 8;
constexpr u32 D3DRS_SHADEMODE = 9;
constexpr u32 D3DRS_ZWRITEENABLE = 14;
constexpr u32 D3DRS_LASTPIXEL = 16;
constexpr u32 D3DRS_SRCBLEND = 19;
constexpr u32 D3DRS_DESTBLEND = 20;
constexpr u32 D3DRS_CULLMODE = 22;
constexpr u32 D3DRS_ZFUNC = 23;
constexpr u32 D3DRS_ALPHAFUNC = 25;
constexpr u32 D3DRS_FOGEND = 37;
constexpr u32 D3DRS_FOGDENSITY = 38;
constexpr u32 D3DRS_STENCILFAIL = 53;
constexpr u32 D3DRS_STENCILZFAIL = 54;
constexpr u32 D3DRS_STENCILPASS = 55;
constexpr u32 D3DRS_STENCILFUNC = 56;
constexpr u32 D3DRS_STENCILMASK = 58;
constexpr u32 D3DRS_STENCILWRITEMASK = 59;
constexpr u32 D3DRS_TEXTUREFACTOR = 60;
constexpr u32 D3DRS_CLIPPING = 136;
constexpr u32 D3DRS_LIGHTING = 137;
constexpr u32 D3DRS_COLORVERTEX = 141;
constexpr u32 D3DRS_LOCALVIEWER = 142;
constexpr u32 D3DRS_DIFFUSEMATERIALSOURCE = 145;
constexpr u32 D3DRS_SPECULARMATERIALSOURCE = 146;
constexpr u32 D3DRS_POINTSIZE = 154;
constexpr u32 D3DRS_POINTSIZE_MIN = 155;
constexpr u32 D3DRS_POINTSCALE_A = 158;
constexpr u32 D3DRS_MULTISAMPLEANTIALIAS = 161;
constexpr u32 D3DRS_MULTISAMPLEMASK = 162;
constexpr u32 D3DRS_POINTSIZE_MAX = 166;
constexpr u32 D3DRS_COLORWRITEENABLE = 168;
constexpr u32 D3DRS_BLENDOP = 171;
constexpr u32 D3DRS_POSITIONDEGREE = 172;
constexpr u32 D3DRS_NORMALDEGREE = 173;
constexpr u32 D3DRS_MINTESSELLATIONLEVEL = 178;
constexpr u32 D3DRS_MAXTESSELLATIONLEVEL = 179;
constexpr u32 D3DRS_ADAPTIVETESS_Z = 182;
constexpr u32 D3DRS_CCW_STENCILFAIL = 186;
constexpr u32 D3DRS_CCW_STENCILZFAIL = 187;
constexpr u32 D3DRS_CCW_STENCILPASS = 188;
constexpr u32 D3DRS_CCW_STENCILFUNC = 189;
constexpr u32 D3DRS_COLORWRITEENABLE1 = 190;
constexpr u32 D3DRS_COLORWRITEENABLE2 = 191;
constexpr u32 D3DRS_COLORWRITEENABLE3 = 192;
constexpr u32 D3DRS_BLENDFACTOR = 193;
constexpr u32 D3DRS_SRCBLENDALPHA = 207;
constexpr u32 D3DRS_DESTBLENDALPHA = 208;
constexpr u32 D3DRS_BLENDOPALPHA = 209;

// D3DTEXTURESTAGESTATETYPE
constexpr u32 D3DTSS_COLOROP = 1;
constexpr u32 D3DTSS_COLORARG1 = 2;
constexpr u32 D3DTSS_COLORARG2 = 3;
constexpr u32 D3DTSS_ALPHAOP = 4;
constexpr u32 D3DTSS_ALPHAARG1 = 5;
constexpr u32 D3DTSS_ALPHAARG2 = 6;
constexpr u32 D3DTSS_TEXCOORDINDEX = 11;
constexpr u32 D3DTSS_COLORARG0 = 26;
constexpr u32 D3DTSS_ALPHAARG0 = 27;
constexpr u32 D3DTSS_RESULTARG = 28;
constexpr u32 D3DTSS_CONSTANT = 32;

// D3DSAMPLERSTATETYPE
constexpr u32 D3DSAMP_ADDRESSU = 1;
constexpr u32 D3DSAMP_ADDRESSV = 2;
constexpr u32 D3DSAMP_ADDRESSW = 3;
constexpr u32 D3DSAMP_BORDERCOLOR = 4;
constexpr u32 D3DSAMP_MAGFILTER = 5;
constexpr u32 D3DSAMP_MINFILTER = 6;
constexpr u32 D3DSAMP_MAXANISOTROPY = 10;

} // namespace

u32 d3d9_default_render_state(u32 state, bool auto_depth_stencil) noexcept {
    switch (state) {
    // D3DZB_TRUE only when the device was created with a depth buffer.
    case D3DRS_ZENABLE:               return auto_depth_stencil ? 1u : 0u;
    case D3DRS_FILLMODE:              return 3;  // D3DFILL_SOLID
    case D3DRS_SHADEMODE:             return 2;  // D3DSHADE_GOURAUD
    case D3DRS_ZWRITEENABLE:          return 1;
    case D3DRS_LASTPIXEL:             return 1;
    case D3DRS_SRCBLEND:              return 2;  // D3DBLEND_ONE
    case D3DRS_DESTBLEND:             return 1;  // D3DBLEND_ZERO
    case D3DRS_CULLMODE:              return 3;  // D3DCULL_CCW
    case D3DRS_ZFUNC:                 return 4;  // D3DCMP_LESSEQUAL
    case D3DRS_ALPHAFUNC:             return 8;  // D3DCMP_ALWAYS
    case D3DRS_FOGEND:                return f32_bits(1.0f);
    case D3DRS_FOGDENSITY:            return f32_bits(1.0f);
    case D3DRS_STENCILFAIL:           return 1;  // D3DSTENCILOP_KEEP
    case D3DRS_STENCILZFAIL:          return 1;
    case D3DRS_STENCILPASS:           return 1;
    case D3DRS_STENCILFUNC:           return 8;  // D3DCMP_ALWAYS
    case D3DRS_STENCILMASK:           return 0xFFFFFFFF;
    case D3DRS_STENCILWRITEMASK:      return 0xFFFFFFFF;
    case D3DRS_TEXTUREFACTOR:         return 0xFFFFFFFF;
    case D3DRS_CLIPPING:              return 1;
    case D3DRS_LIGHTING:              return 1;
    case D3DRS_COLORVERTEX:           return 1;
    case D3DRS_LOCALVIEWER:           return 1;
    case D3DRS_DIFFUSEMATERIALSOURCE: return 1;  // D3DMCS_COLOR1
    case D3DRS_SPECULARMATERIALSOURCE:return 2;  // D3DMCS_COLOR2
    case 147: /* D3DRS_AMBIENTMATERIALSOURCE */ return 0;  // D3DMCS_MATERIAL
    case 148: /* D3DRS_EMISSIVEMATERIALSOURCE */ return 0;  // D3DMCS_MATERIAL
    case D3DRS_POINTSIZE:             return f32_bits(1.0f);
    case D3DRS_POINTSIZE_MIN:         return f32_bits(1.0f);
    case D3DRS_POINTSCALE_A:          return f32_bits(1.0f);
    case D3DRS_MULTISAMPLEANTIALIAS:  return 1;
    case D3DRS_MULTISAMPLEMASK:       return 0xFFFFFFFF;
    case D3DRS_POINTSIZE_MAX:         return f32_bits(64.0f);
    case D3DRS_COLORWRITEENABLE:      return 0x0000000F;
    case D3DRS_BLENDOP:               return 1;  // D3DBLENDOP_ADD
    case D3DRS_POSITIONDEGREE:        return 3;  // D3DDEGREE_CUBIC
    case D3DRS_NORMALDEGREE:          return 1;  // D3DDEGREE_LINEAR
    case D3DRS_MINTESSELLATIONLEVEL:  return f32_bits(1.0f);
    case D3DRS_MAXTESSELLATIONLEVEL:  return f32_bits(1.0f);
    case D3DRS_ADAPTIVETESS_Z:        return f32_bits(1.0f);
    case D3DRS_CCW_STENCILFAIL:       return 1;
    case D3DRS_CCW_STENCILZFAIL:      return 1;
    case D3DRS_CCW_STENCILPASS:       return 1;
    case D3DRS_CCW_STENCILFUNC:       return 8;
    case D3DRS_COLORWRITEENABLE1:     return 0x0000000F;
    case D3DRS_COLORWRITEENABLE2:     return 0x0000000F;
    case D3DRS_COLORWRITEENABLE3:     return 0x0000000F;
    case D3DRS_BLENDFACTOR:           return 0xFFFFFFFF;
    case D3DRS_SRCBLENDALPHA:         return 2;  // D3DBLEND_ONE
    case D3DRS_DESTBLENDALPHA:        return 1;  // D3DBLEND_ZERO
    case D3DRS_BLENDOPALPHA:          return 1;  // D3DBLENDOP_ADD
    default:
        // Every remaining render state starts at zero: the disable/false/none
        // value for its enumeration.
        return 0;
    }
}

u32 d3d9_default_texture_stage_state(u32 stage, u32 type) noexcept {
    switch (type) {
    // Stage 0 starts modulating the texture with the diffuse colour; every
    // later stage starts disabled, which is what ends the cascade.
    case D3DTSS_COLOROP:   return stage == 0 ? 4u : 1u; // MODULATE : DISABLE
    case D3DTSS_ALPHAOP:   return stage == 0 ? 2u : 1u; // SELECTARG1 : DISABLE
    case D3DTSS_COLORARG1: return 2;  // D3DTA_TEXTURE
    case D3DTSS_COLORARG2: return 1;  // D3DTA_CURRENT
    case D3DTSS_ALPHAARG1: return 2;  // D3DTA_TEXTURE
    case D3DTSS_ALPHAARG2: return 1;  // D3DTA_CURRENT
    case D3DTSS_COLORARG0: return 1;  // D3DTA_CURRENT
    case D3DTSS_ALPHAARG0: return 1;  // D3DTA_CURRENT
    case D3DTSS_RESULTARG: return 1;  // D3DTA_CURRENT
    case D3DTSS_CONSTANT: return 0xFFFFFFFFu;
    case D3DTSS_TEXCOORDINDEX: return stage;
    default: return 0;
    }
}

u32 d3d9_default_sampler_state(u32 type) noexcept {
    switch (type) {
    case D3DSAMP_ADDRESSU:     return 1;  // D3DTADDRESS_WRAP
    case D3DSAMP_ADDRESSV:     return 1;
    case D3DSAMP_ADDRESSW:     return 1;
    case D3DSAMP_BORDERCOLOR:  return 0x00000000;  // D3DCOLOR black
    case D3DSAMP_MAGFILTER:    return 1;  // D3DTEXF_POINT
    case D3DSAMP_MINFILTER:    return 1;  // D3DTEXF_POINT
    case D3DSAMP_MAXANISOTROPY:return 1;
    default: return 0;
    }
}

u32 d3d9_format_bytes_per_pixel(u32 format) noexcept {
    switch (format) {
    case 20: return 3;  // D3DFMT_R8G8B8
    case 21: return 4;  // D3DFMT_A8R8G8B8
    case 22: return 4;  // D3DFMT_X8R8G8B8
    case 23: return 2;  // D3DFMT_R5G6B5
    case 24: return 2;  // D3DFMT_X1R5G5B5
    case 25: return 2;  // D3DFMT_A1R5G5B5
    case 26: return 2;  // D3DFMT_A4R4G4B4
    case 28: return 1;  // D3DFMT_A8
    case 30: return 2;  // D3DFMT_X4R4G4B4
    case 31: return 4;  // D3DFMT_A2B10G10R10
    case 32: return 4;  // D3DFMT_A8B8G8R8
    case 33: return 4;  // D3DFMT_X8B8G8R8
    case 40: return 2;  // D3DFMT_A8P8 — Learn: index + alpha
    case 41: return 1;  // D3DFMT_P8
    case 50: return 1;  // D3DFMT_L8
    case 51: return 2;  // D3DFMT_A8L8 — Learn: high byte alpha, low luminance
    case 34: return 4;  // D3DFMT_G16R16
    case 36: return 8;  // D3DFMT_A16B16G16R16
    case 60: return 2;  // D3DFMT_V8U8
    case 61: return 2;  // D3DFMT_L6V5U5
    case 62: return 4;  // D3DFMT_X8L8V8U8
    case 63: return 4;  // D3DFMT_Q8W8V8U8
    case 64: return 4;  // D3DFMT_V16U16
    case 81: return 2;  // D3DFMT_L16
    case 111: return 2; // D3DFMT_R16F
    case 112: return 4; // D3DFMT_G16R16F
    case 113: return 8; // D3DFMT_A16B16G16R16F
    case 114: return 4; // D3DFMT_R32F
    case 115: return 8; // D3DFMT_G32R32F
    case 116: return 16; // D3DFMT_A32B32G32R32F
    case 117: return 2; // D3DFMT_CxV8U8
    case 101: return 2; // D3DFMT_INDEX16
    case 102: return 4; // D3DFMT_INDEX32
    default:
        // Compressed and depth formats have no single byte-per-pixel value,
        // and an unknown format has none at all.
        return 0;
    }
}

namespace {

constexpr u32 kDxt1 = 0x31545844u; // 'DXT1'
constexpr u32 kDxt2 = 0x32545844u;
constexpr u32 kDxt3 = 0x33545844u;
constexpr u32 kDxt4 = 0x34545844u;
constexpr u32 kDxt5 = 0x35545844u;

[[nodiscard]] u32 dxt_block_bytes(u32 format) noexcept {
    switch (format) {
    case kDxt1: return 8;
    case kDxt2:
    case kDxt3:
    case kDxt4:
    case kDxt5: return 16;
    default: return 0;
    }
}

} // namespace

bool d3d9_surface_storage(u32 format, u32 width, u32 height, u32& pitch_out,
                          usize& bytes_out) noexcept {
    const u32 bpp = d3d9_format_bytes_per_pixel(format);
    if (bpp != 0) {
        pitch_out = width * bpp;
        bytes_out = static_cast<usize>(pitch_out) * height;
        return true;
    }
    const u32 block = dxt_block_bytes(format);
    if (block == 0 || width == 0 || height == 0) return false;
    const u32 blocks_w = (width + 3u) / 4u;
    const u32 blocks_h = (height + 3u) / 4u;
    pitch_out = blocks_w * block;
    bytes_out = static_cast<usize>(pitch_out) * blocks_h;
    return true;
}

bool d3d9_volume_storage(u32 format, u32 width, u32 height, u32 depth,
                         u32& row_pitch_out, u32& slice_pitch_out,
                         usize& bytes_out) noexcept {
    if (width == 0 || height == 0 || depth == 0) return false;
    u32 row_pitch = 0;
    usize slice_bytes = 0;
    if (!d3d9_surface_storage(format, width, height, row_pitch, slice_bytes)) {
        return false;
    }
    row_pitch_out = row_pitch;
    slice_pitch_out = static_cast<u32>(slice_bytes);
    bytes_out = slice_bytes * depth;
    return true;
}

graphics::PixelFormat d3d9_backend_format(u32 format) noexcept {
    switch (format) {
    case 21: // D3DFMT_A8R8G8B8
    case 22: // D3DFMT_X8R8G8B8
        return graphics::PixelFormat::BGRA8Unorm;
    case 20: // D3DFMT_R8G8B8 — expanded to BGRA8 on unlock upload
    case 23: // D3DFMT_R5G6B5
    case 24: // D3DFMT_X1R5G5B5
    case 25: // D3DFMT_A1R5G5B5
    case 26: // D3DFMT_A4R4G4B4
    case 30: // D3DFMT_X4R4G4B4
        return graphics::PixelFormat::BGRA8Unorm;
    case 32: // D3DFMT_A8B8G8R8
    case 33: // D3DFMT_X8B8G8R8
        return graphics::PixelFormat::RGBA8Unorm;
    case 31: // D3DFMT_A2B10G10R10
        return graphics::PixelFormat::RGB10A2Unorm;
    case 28: // D3DFMT_A8
    case 50: // D3DFMT_L8
        return graphics::PixelFormat::R8Unorm;
    case 40: // D3DFMT_A8P8 — expanded through device palette to BGRA8
    case 41: // D3DFMT_P8
        return graphics::PixelFormat::BGRA8Unorm;
    case 51: // D3DFMT_A8L8 — L in R, A in G (Learn bit order)
        return graphics::PixelFormat::RG8Unorm;
    case 60: // D3DFMT_V8U8
        return graphics::PixelFormat::RG8Snorm;
    // D3DFMT_L6V5U5 has an exact expansion helper below, but remains
    // unadvertised until a Windows adapter/oracle proves the complete path.
    case 62: // D3DFMT_X8L8V8U8 — expanded to float {DU,DV,L,1}
        return graphics::PixelFormat::RGBA32Float;
    case 63: // D3DFMT_Q8W8V8U8
        return graphics::PixelFormat::RGBA8Snorm;
    case 64: // D3DFMT_V16U16 — expanded to float {DU,DV,0,1}
        return graphics::PixelFormat::RGBA32Float;
    case 117: // D3DFMT_CxV8U8 — expand C=sqrt(1-U^2-V^2)
        return graphics::PixelFormat::RGBA32Float;
    case 34: // D3DFMT_G16R16
        return graphics::PixelFormat::RG16Unorm;
    case 36: // D3DFMT_A16B16G16R16
        return graphics::PixelFormat::RGBA16Unorm;
    case 81: // D3DFMT_L16
        return graphics::PixelFormat::R16Unorm;
    case 111: // D3DFMT_R16F
        return graphics::PixelFormat::R16Float;
    case 112: // D3DFMT_G16R16F
        return graphics::PixelFormat::RG16Float;
    case 113: // D3DFMT_A16B16G16R16F
        return graphics::PixelFormat::RGBA16Float;
    case 114: // D3DFMT_R32F
        return graphics::PixelFormat::R32Float;
    case 115: // D3DFMT_G32R32F
        return graphics::PixelFormat::RG32Float;
    case 116: // D3DFMT_A32B32G32R32F
        return graphics::PixelFormat::RGBA32Float;
    case kDxt1:
        return graphics::PixelFormat::BC1_RGBA;
    case kDxt2:
    case kDxt3:
        return graphics::PixelFormat::BC2_RGBA;
    case kDxt4:
    case kDxt5:
        return graphics::PixelFormat::BC3_RGBA;
    // Depth formats: guest D24S8/D24X8 map to Metal-friendly D32F+S8 (same as
    // the swap-chain depth attachment). Guest format on the COM object is
    // unchanged.
    case 70: // D3DFMT_D16_LOCKABLE
    case 80: // D3DFMT_D16
        return graphics::PixelFormat::D16Unorm;
    case 71: // D3DFMT_D32
    case 82: // D3DFMT_D32F_LOCKABLE
        return graphics::PixelFormat::D32Float;
    case 75: // D3DFMT_D24S8
    case 77: // D3DFMT_D24X8
    case 83: // D3DFMT_D24FS8
        return graphics::PixelFormat::D32Float_S8Uint;
    default:
        // Everything else needs an expansion or swizzle step that the shared
        // core does not expose yet; claiming a format here would silently
        // render wrong pixels.
        return graphics::PixelFormat::Unknown;
    }
}

bool d3d9_format_supported(u32 format) noexcept {
    // CheckDeviceFormat must match CreateTexture: only formats with a shared
    // core pixel format (or index-buffer formats) are advertised.
    if (d3d9_backend_format(format) != graphics::PixelFormat::Unknown) {
        return true;
    }
    switch (format) {
    case D3DFMT_INDEX16:
    case D3DFMT_INDEX32:
        return true;
    default:
        return false;
    }
}

namespace {

[[nodiscard]] u8 expand5(u32 v) noexcept {
    return static_cast<u8>((v * 255u + 15u) / 31u);
}

[[nodiscard]] u8 expand6(u32 v) noexcept {
    return static_cast<u8>((v * 255u + 31u) / 63u);
}

[[nodiscard]] u8 expand4(u32 v) noexcept {
    return static_cast<u8>(v * 17u);
}

} // namespace

bool d3d9_expand_lock_to_bgra8(u32 format, u32 width, u32 height,
                               const u8* src, u32 src_pitch,
                               std::vector<u8>& out) noexcept {
    if (src == nullptr || width == 0 || height == 0) return false;
    const u32 guest_bpp = d3d9_format_bytes_per_pixel(format);
    if (guest_bpp == 0 || src_pitch < width * guest_bpp) return false;

    // Native BGRA8 / X8R8G8B8 / A8R8G8B8 need no expansion.
    if (format == 21 || format == 22) return false;

    switch (format) {
    case 20: // R8G8B8
    case 23: // R5G6B5
    case 24: // X1R5G5B5
    case 25: // A1R5G5B5
    case 26: // A4R4G4B4
    case 30: // X4R4G4B4
        break;
    default:
        return false;
    }

    out.resize(static_cast<usize>(width) * height * 4u);
    for (u32 y = 0; y < height; ++y) {
        const u8* row = src + static_cast<usize>(y) * src_pitch;
        u8* dst = out.data() + static_cast<usize>(y) * width * 4u;
        for (u32 x = 0; x < width; ++x) {
            u8 b = 0, g = 0, r = 0, a = 0xFF;
            switch (format) {
            case 20: { // R8G8B8 stored as B,G,R
                const u8* p = row + static_cast<usize>(x) * 3u;
                b = p[0];
                g = p[1];
                r = p[2];
                break;
            }
            case 23: { // R5G6B5
                const u16 p = static_cast<u16>(
                    row[static_cast<usize>(x) * 2u] |
                    (static_cast<u16>(row[static_cast<usize>(x) * 2u + 1u])
                     << 8));
                r = expand5((p >> 11) & 0x1Fu);
                g = expand6((p >> 5) & 0x3Fu);
                b = expand5(p & 0x1Fu);
                break;
            }
            case 24: // X1R5G5B5
            case 25: { // A1R5G5B5
                const u16 p = static_cast<u16>(
                    row[static_cast<usize>(x) * 2u] |
                    (static_cast<u16>(row[static_cast<usize>(x) * 2u + 1u])
                     << 8));
                a = (format == 25 && (p & 0x8000u) == 0) ? 0u : 0xFFu;
                r = expand5((p >> 10) & 0x1Fu);
                g = expand5((p >> 5) & 0x1Fu);
                b = expand5(p & 0x1Fu);
                break;
            }
            case 26: // A4R4G4B4
            case 30: { // X4R4G4B4
                const u16 p = static_cast<u16>(
                    row[static_cast<usize>(x) * 2u] |
                    (static_cast<u16>(row[static_cast<usize>(x) * 2u + 1u])
                     << 8));
                a = (format == 30) ? 0xFFu : expand4((p >> 12) & 0xFu);
                r = expand4((p >> 8) & 0xFu);
                g = expand4((p >> 4) & 0xFu);
                b = expand4(p & 0xFu);
                break;
            }
            default:
                break;
            }
            dst[static_cast<usize>(x) * 4u + 0] = b;
            dst[static_cast<usize>(x) * 4u + 1] = g;
            dst[static_cast<usize>(x) * 4u + 2] = r;
            dst[static_cast<usize>(x) * 4u + 3] = a;
        }
    }
    return true;
}

bool d3d9_expand_special_bump_to_rgba32f(
    u32 format, u32 width, u32 height, const u8* src, u32 src_pitch,
    std::vector<u8>& out) noexcept {
    if ((format != 61 && format != 62 && format != 64 && format != 117) ||
        src == nullptr || width == 0 || height == 0) {
        return false;
    }
    const u32 guest_bpp = d3d9_format_bytes_per_pixel(format);
    if (src_pitch < width * guest_bpp) return false;

    out.resize(static_cast<usize>(width) * height * 4u * sizeof(float));
    const auto snorm5 = [](u32 bits) noexcept {
        const int value = (bits & 0x10u) != 0
                              ? static_cast<int>(bits) - 32
                              : static_cast<int>(bits);
        return std::max(-1.0f, static_cast<float>(value) / 15.0f);
    };
    const auto snorm8 = [](u8 bits) noexcept {
        const int value = bits >= 128u ? static_cast<int>(bits) - 256
                                       : static_cast<int>(bits);
        return std::max(-1.0f, static_cast<float>(value) / 127.0f);
    };
    const auto snorm16 = [](u16 bits) noexcept {
        const int value = bits >= 32768u ? static_cast<int>(bits) - 65536
                                         : static_cast<int>(bits);
        return std::max(-1.0f, static_cast<float>(value) / 32767.0f);
    };
    for (u32 y = 0; y < height; ++y) {
        const u8* row = src + static_cast<usize>(y) * src_pitch;
        for (u32 x = 0; x < width; ++x) {
            float du = 0.0f, dv = 0.0f, luminance = 0.0f;
            if (format == 61) {
                const usize off = static_cast<usize>(x) * 2u;
                const u16 packed = static_cast<u16>(
                    row[off] | (static_cast<u16>(row[off + 1u]) << 8));
                du = snorm5(packed & 0x1Fu);
                dv = snorm5((packed >> 5u) & 0x1Fu);
                luminance = static_cast<float>((packed >> 10u) & 0x3Fu) /
                            63.0f;
            } else if (format == 62) {
                const usize off = static_cast<usize>(x) * 4u;
                // D3DFORMAT names bits MSB-to-LSB, so little-endian memory is
                // U, V, L, X.
                du = snorm8(row[off]);
                dv = snorm8(row[off + 1u]);
                luminance = static_cast<float>(row[off + 2u]) / 255.0f;
            } else if (format == 64) {
                const usize off = static_cast<usize>(x) * 4u;
                const u16 u = static_cast<u16>(
                    row[off] | (static_cast<u16>(row[off + 1u]) << 8u));
                const u16 v = static_cast<u16>(
                    row[off + 2u] |
                    (static_cast<u16>(row[off + 3u]) << 8u));
                du = snorm16(u);
                dv = snorm16(v);
            } else {
                const usize off = static_cast<usize>(x) * 2u;
                du = snorm8(row[off]);
                dv = snorm8(row[off + 1u]);
                luminance = std::sqrt(std::max(0.0f, 1.0f - du * du -
                                                        dv * dv));
            }
            const usize dst_off =
                (static_cast<usize>(y) * width + x) * 4u;
            const float texel[4] = {du, dv, luminance, 1.0f};
            std::memcpy(out.data() + dst_off * sizeof(float), texel,
                        sizeof(texel));
        }
    }
    return true;
}

bool d3d9_is_palettized_format(u32 format) noexcept {
    return format == D3DFMT_P8 || format == D3DFMT_A8P8;
}

bool d3d9_expand_palettized_to_bgra8(u32 format, u32 width, u32 height,
                                     const u8* src, u32 src_pitch,
                                     const u8* palette_entries,
                                     std::vector<u8>& out) noexcept {
    if (!d3d9_is_palettized_format(format) || src == nullptr || width == 0 ||
        height == 0) {
        return false;
    }
    const u32 guest_bpp = d3d9_format_bytes_per_pixel(format);
    if (guest_bpp == 0 || src_pitch < width * guest_bpp) return false;

    // Learn: Texture Palettes — never-set palettes default to 0xFFFFFFFF.
    alignas(4) u8 default_palette[D3D9Manager::DeviceState::kPaletteBytes];
    const u8* palette = palette_entries;
    if (palette == nullptr) {
        std::memset(default_palette, 0xFF, sizeof(default_palette));
        palette = default_palette;
    }

    out.resize(static_cast<usize>(width) * height * 4u);
    for (u32 y = 0; y < height; ++y) {
        const u8* row = src + static_cast<usize>(y) * src_pitch;
        u8* dst = out.data() + static_cast<usize>(y) * width * 4u;
        for (u32 x = 0; x < width; ++x) {
            u8 index = 0;
            u8 a = 0xFF;
            if (format == D3DFMT_A8P8) {
                // Learn D3DFMT_A8P8: low byte palette index, high byte alpha
                // (same packing convention as A8L8).
                const usize off = static_cast<usize>(x) * 2u;
                index = row[off];
                a = row[off + 1u];
            } else {
                index = row[x];
            }
            const u8* pe = palette + static_cast<usize>(index) * 4u;
            // PALETTEENTRY: peRed, peGreen, peBlue, peFlags(alpha).
            // Backend BGRA8Unorm upload order: B, G, R, A.
            dst[static_cast<usize>(x) * 4u + 0] = pe[2];
            dst[static_cast<usize>(x) * 4u + 1] = pe[1];
            dst[static_cast<usize>(x) * 4u + 2] = pe[0];
            if (format == D3DFMT_A8P8) {
                // Learn D3DFMT_A8P8: per-pixel alpha; ignore palette peFlags.
                dst[static_cast<usize>(x) * 4u + 3] = a;
            } else {
                // Learn: with D3DPTEXTURECAPS_ALPHAPALETTE, P8 alpha is
                // PALETTEENTRY.peFlags (D3D9 peFlags = alpha intensity).
                dst[static_cast<usize>(x) * 4u + 3] = pe[3];
            }
        }
    }
    return true;
}

void fill_d3d9_device_caps(std::array<u8, D3DCAPS9_SIZE>& caps,
                           u32 device_type, u32 adapter) noexcept {
    caps.fill(0);
    const auto write_u32 = [&caps](usize offset, u32 value) {
        std::memcpy(caps.data() + offset, &value, sizeof(value));
    };
    const auto write_f32 = [&caps](usize offset, float value) {
        std::memcpy(caps.data() + offset, &value, sizeof(value));
    };

    // Capability bit values match d3d9caps.h. Reported level is a typical
    // DX9 SM3 HAL: enough for a Direct3D 9 guest to pass min-spec
    // checks without inventing features the frontend cannot yet execute.
    constexpr u32 filter_caps =
        0x01000000u | 0x02000000u | 0x04000000u | // MAGF POINT/LINEAR/ANISO
        0x00010000u | 0x00020000u |               // MIPF POINT/LINEAR
        0x00000100u | 0x00000200u | 0x00000400u;  // MINF POINT/LINEAR/ANISO
    constexpr u32 address_caps =
        0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u; // WRAP..MIRRORONCE
    constexpr u32 cmp_caps = 0xFFu;
    constexpr u32 blend_caps =
        0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | 0x80u |
        0x100u | 0x200u | 0x400u | 0x800u | 0x1000u | 0x2000u;
    constexpr u32 texture_op_caps =
        0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | 0x80u |
        0x100u | 0x200u | 0x400u | 0x800u | 0x1000u | 0x2000u | 0x4000u |
        0x8000u | 0x10000u | 0x20000u | 0x40000u | 0x80000u | 0x100000u |
        0x200000u | 0x400000u | 0x800000u | 0x1000000u | 0x2000000u;

    write_u32(D3DCAPS9_OFFSET_DEVICE_TYPE, device_type);
    write_u32(D3DCAPS9_OFFSET_ADAPTER_ORDINAL, adapter);
    write_u32(D3DCAPS9_OFFSET_CAPS, 0x20000u); // D3DCAPS_READ_SCANLINE
    // Caps2: D3DCAPS2_FULLSCREENGAMMA — present applies SetGammaRamp as a
    // LUT (host cannot program the display DAC). Omit CANCALIBRATEGAMMA.
    write_u32(D3DCAPS9_OFFSET_CAPS2,
              0x00000002u | 0x00020000u | 0x10000000u | 0x40000000u);
    write_u32(D3DCAPS9_OFFSET_CAPS3,
              0x20u | 0x100u | 0x200u); // ALPHA_FLIP_OR_DISCARD|COPY_TO_*
    write_u32(D3DCAPS9_OFFSET_PRESENTATION_INTERVALS,
              0x1u | 0x80000000u); // ONE | IMMEDIATE (DEFAULT is 0)
    write_u32(D3DCAPS9_OFFSET_CURSOR_CAPS, 0x1u); // COLOR
    write_u32(D3DCAPS9_OFFSET_DEV_CAPS,
              0x00000010u | 0x00000020u | 0x00000040u | 0x00000080u |
                  0x00000100u | 0x00000200u | 0x00000400u | 0x00000800u |
                  0x00002000u | 0x00008000u | 0x00010000u | 0x00020000u |
                  0x00080000u | 0x00100000u); // HWTnL|HWRASTER|PURE|…
    write_u32(D3DCAPS9_OFFSET_PRIMITIVE_MISC_CAPS,
              0x2u | 0x10u | 0x20u | 0x40u | 0x80u | 0x100u | 0x200u |
                  0x400u | 0x800u | 0x4000u | 0x8000u | 0x10000u |
                  0x20000u | 0x40000u | 0x80000u);
    write_u32(D3DCAPS9_OFFSET_RASTER_CAPS,
              0x1u | 0x10u | 0x80u | 0x100u | 0x2000u | 0x10000u |
                  0x20000u | 0x100000u | 0x200000u | 0x400000u |
                  0x1000000u | 0x2000000u | 0x4000000u);
    write_u32(D3DCAPS9_OFFSET_ZCMP_CAPS, cmp_caps);
    write_u32(D3DCAPS9_OFFSET_SRC_BLEND_CAPS, blend_caps);
    write_u32(D3DCAPS9_OFFSET_DEST_BLEND_CAPS, blend_caps);
    write_u32(D3DCAPS9_OFFSET_ALPHA_CMP_CAPS, cmp_caps);
    write_u32(D3DCAPS9_OFFSET_SHADE_CAPS,
              0x8u | 0x200u | 0x4000u | 0x80000u); // GOURAUD color/spec/alpha/fog
    write_u32(D3DCAPS9_OFFSET_TEXTURE_CAPS,
              0x1u | 0x4u | 0x40u | 0x80u | 0x100u | 0x400u | 0x800u |
                  0x2000u | 0x4000u | 0x8000u |
                  0x10000u); // perspective/alpha/ALPHAPALETTE/mip/cube/vol…
    write_u32(D3DCAPS9_OFFSET_TEXTURE_FILTER_CAPS, filter_caps);
    write_u32(D3DCAPS9_OFFSET_CUBE_TEXTURE_FILTER_CAPS, filter_caps);
    write_u32(D3DCAPS9_OFFSET_VOLUME_TEXTURE_FILTER_CAPS, filter_caps);
    write_u32(D3DCAPS9_OFFSET_TEXTURE_ADDRESS_CAPS, address_caps);
    write_u32(D3DCAPS9_OFFSET_VOLUME_TEXTURE_ADDRESS_CAPS, address_caps);
    write_u32(D3DCAPS9_OFFSET_LINE_CAPS,
              0x1u | 0x2u | 0x4u | 0x8u | 0x10u);
    write_u32(D3DCAPS9_OFFSET_MAX_TEXTURE_WIDTH, 8192);
    write_u32(D3DCAPS9_OFFSET_MAX_TEXTURE_HEIGHT, 8192);
    write_u32(D3DCAPS9_OFFSET_MAX_VOLUME_EXTENT, 256);
    write_u32(D3DCAPS9_OFFSET_MAX_TEXTURE_REPEAT, 8192);
    write_u32(D3DCAPS9_OFFSET_MAX_TEXTURE_ASPECT_RATIO, 8192);
    write_u32(D3DCAPS9_OFFSET_MAX_ANISOTROPY, 16);
    write_f32(D3DCAPS9_OFFSET_MAX_VERTEX_W, 1.0e10f);
    write_f32(D3DCAPS9_OFFSET_GUARD_BAND_LEFT, -1.0e9f);
    write_f32(D3DCAPS9_OFFSET_GUARD_BAND_TOP, -1.0e9f);
    write_f32(D3DCAPS9_OFFSET_GUARD_BAND_RIGHT, 1.0e9f);
    write_f32(D3DCAPS9_OFFSET_GUARD_BAND_BOTTOM, 1.0e9f);
    write_f32(D3DCAPS9_OFFSET_EXTENTS_ADJUST, 0.0f);
    write_u32(D3DCAPS9_OFFSET_STENCIL_CAPS,
              0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | 0x80u |
                  0x100u);
    write_u32(D3DCAPS9_OFFSET_FVF_CAPS, 0x0008FFFFu); // 8 texcoords + DONOTSTRIP
    write_u32(D3DCAPS9_OFFSET_TEXTURE_OP_CAPS, texture_op_caps);
    write_u32(D3DCAPS9_OFFSET_MAX_TEXTURE_BLEND_STAGES, 8);
    write_u32(D3DCAPS9_OFFSET_MAX_SIMULTANEOUS_TEXTURES, 8);
    write_u32(D3DCAPS9_OFFSET_VERTEX_PROCESSING_CAPS,
              0x1u | 0x2u | 0x8u | 0x10u | 0x20u | 0x100u);
    write_u32(D3DCAPS9_OFFSET_MAX_ACTIVE_LIGHTS, 8);
    write_u32(D3DCAPS9_OFFSET_MAX_USER_CLIP_PLANES, 6);
    write_u32(D3DCAPS9_OFFSET_MAX_VERTEX_BLEND_MATRICES, 4);
    write_u32(D3DCAPS9_OFFSET_MAX_VERTEX_BLEND_MATRIX_INDEX, 8);
    write_f32(D3DCAPS9_OFFSET_MAX_POINT_SIZE, 256.0f);
    write_u32(D3DCAPS9_OFFSET_MAX_PRIMITIVE_COUNT, 0xFFFFFFu);
    write_u32(D3DCAPS9_OFFSET_MAX_VERTEX_INDEX, 0xFFFFFFu);
    write_u32(D3DCAPS9_OFFSET_MAX_STREAMS, 16);
    write_u32(D3DCAPS9_OFFSET_MAX_STREAM_STRIDE, 255);
    write_u32(D3DCAPS9_OFFSET_VERTEX_SHADER_VERSION, 0xFFFE0300u); // vs_3_0
    write_u32(D3DCAPS9_OFFSET_MAX_VERTEX_SHADER_CONST, 256);
    write_u32(D3DCAPS9_OFFSET_PIXEL_SHADER_VERSION, 0xFFFF0300u); // ps_3_0
    write_f32(D3DCAPS9_OFFSET_PIXEL_SHADER_1X_MAX_VALUE, 8.0f);
    write_u32(D3DCAPS9_OFFSET_DEV_CAPS2,
              0x1u | 0x10u | 0x40u); // STREAMOFFSET|STRETCHRECT|SHAREOFFSET
    write_f32(D3DCAPS9_OFFSET_MAX_NPATCH_TESSELLATION_LEVEL, 1.0f);
    write_u32(D3DCAPS9_OFFSET_MASTER_ADAPTER_ORDINAL, adapter);
    write_u32(D3DCAPS9_OFFSET_ADAPTER_ORDINAL_IN_GROUP, 0);
    write_u32(D3DCAPS9_OFFSET_NUMBER_OF_ADAPTERS_IN_GROUP, 1);
    write_u32(D3DCAPS9_OFFSET_DECL_TYPES,
              0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x100u | 0x200u);
    write_u32(D3DCAPS9_OFFSET_NUM_SIMULTANEOUS_RTS, 4);
    write_u32(D3DCAPS9_OFFSET_STRETCH_RECT_FILTER_CAPS,
              0x02000000u | 0x00000200u); // MAG/MIN LINEAR
    // D3DVSHADERCAPS2_0
    write_u32(D3DCAPS9_OFFSET_VS20_CAPS + 0, 0x1u); // PREDICATION
    write_u32(D3DCAPS9_OFFSET_VS20_CAPS + 4, 24);   // DynamicFlowControlDepth
    write_u32(D3DCAPS9_OFFSET_VS20_CAPS + 8, 32);   // NumTemps
    write_u32(D3DCAPS9_OFFSET_VS20_CAPS + 12, 4);   // StaticFlowControlDepth
    // D3DPSHADERCAPS2_0
    write_u32(D3DCAPS9_OFFSET_PS20_CAPS + 0, 0x1Fu);
    write_u32(D3DCAPS9_OFFSET_PS20_CAPS + 4, 24);
    write_u32(D3DCAPS9_OFFSET_PS20_CAPS + 8, 32);
    write_u32(D3DCAPS9_OFFSET_PS20_CAPS + 12, 4);
    write_u32(D3DCAPS9_OFFSET_PS20_CAPS + 16, 512); // NumInstructionSlots
    // Vertex texture filtering matches pixel textures except that Learn
    // explicitly disallows anisotropic minification/magnification.
    write_u32(D3DCAPS9_OFFSET_VERTEX_TEXTURE_FILTER_CAPS,
              filter_caps & ~(0x04000000u | 0x00000400u));
    write_u32(D3DCAPS9_OFFSET_MAX_VSHADER_INSTRUCTIONS_EXECUTED, 0xFFFFFFFFu);
    write_u32(D3DCAPS9_OFFSET_MAX_PSHADER_INSTRUCTIONS_EXECUTED, 0xFFFFFFFFu);
    write_u32(D3DCAPS9_OFFSET_MAX_VERTEX_SHADER_30_INSTRUCTION_SLOTS, 512);
    write_u32(D3DCAPS9_OFFSET_MAX_PIXEL_SHADER_30_INSTRUCTION_SLOTS, 512);
}

namespace {

[[nodiscard]] bool backend_supports_samples(
    const graphics::GraphicsBackendDevice* backend, unsigned count) {
    if (count <= 1) return true;
    return backend != nullptr && backend->supports_texture_sample_count(count);
}

/// NONMASKABLE quality indexes these in order, skipping unsupported counts.
constexpr unsigned kNonmaskableModes[] = {2, 4, 8};

} // namespace

unsigned d3d9_raster_sample_count(
    u32 multi_sample_type, u32 multi_sample_quality,
    const graphics::GraphicsBackendDevice* backend) noexcept {
    if (multi_sample_type == D3DMULTISAMPLE_NONE) return 1;
    if (multi_sample_type == D3DMULTISAMPLE_NONMASKABLE) {
        unsigned index = 0;
        for (unsigned mode : kNonmaskableModes) {
            if (!backend_supports_samples(backend, mode)) continue;
            if (index == multi_sample_quality) return mode;
            ++index;
        }
        return 2;
    }
    if (multi_sample_type >= 2 && multi_sample_type <= 16) {
        return static_cast<unsigned>(multi_sample_type);
    }
    return 1;
}

bool d3d9_multisample_supported(
    const graphics::GraphicsBackendDevice* backend,
    u32 multi_sample_type) noexcept {
    if (multi_sample_type == D3DMULTISAMPLE_NONE) return true;
    if (multi_sample_type == D3DMULTISAMPLE_NONMASKABLE) {
        for (unsigned mode : kNonmaskableModes) {
            if (backend_supports_samples(backend, mode)) return true;
        }
        return false;
    }
    if (multi_sample_type >= 2 && multi_sample_type <= 16) {
        return backend_supports_samples(backend,
                                        static_cast<unsigned>(multi_sample_type));
    }
    return false;
}

u32 d3d9_multisample_quality_levels(
    const graphics::GraphicsBackendDevice* backend,
    u32 multi_sample_type) noexcept {
    if (!d3d9_multisample_supported(backend, multi_sample_type)) return 0;
    if (multi_sample_type == D3DMULTISAMPLE_NONMASKABLE) {
        u32 levels = 0;
        for (unsigned mode : kNonmaskableModes) {
            if (backend_supports_samples(backend, mode)) ++levels;
        }
        return levels;
    }
    return 1;
}

} // namespace vitreo::winapi
