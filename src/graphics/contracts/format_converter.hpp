// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file format_converter.hpp
/// @brief PixelFormat table (format_table), DXGI -> PixelFormat mapping, and
///        Metal pixel-format size helpers (compressed_block_bytes,
///        bytes_per_pixel).

#include <vitreo/common.hpp>

namespace vitreo::graphics {

// =============================================================================
// Pixel Format Enumeration
// =============================================================================

enum class PixelFormat : u32 {
    Unknown = 0,

    // 8-bit
    A8Unorm,
    R8Unorm,
    R8Snorm,

    // 16-bit
    R16Unorm,
    R16Float,
    RG8Unorm,
    D16Unorm,

    // 32-bit
    R32Float,
    RG16Unorm,
    RG16Float,
    RGBA8Unorm,
    RGBA8Unorm_sRGB,
    BGRA8Unorm,
    BGRA8Unorm_sRGB,
    D24Unorm_S8Uint,
    D32Float,
    Stencil8,
    X32_Stencil8,
    X24_Stencil8,

    // 64-bit
    RG32Float,
    RGBA16Unorm,
    RGBA16Float,

    // 128-bit
    RGBA32Float,

    // Compressed (BC / S3TC)
    BC1_RGBA,   // DXT1
    BC2_RGBA,   // DXT3
    BC3_RGBA,   // DXT5
    BC7_RGBAUnorm,
    // The rest of the BC family. BC4/BC5 are the one- and two-channel block
    // formats (Learn, Texture Block Compression in Direct3D 11); BC5 is what
    // every modern engine stores tangent-space normal maps in, so a guest that
    // cannot create a BC5 texture cannot load its materials. BC6H is the HDR
    // block format. The _sRGB variants differ from their linear counterparts
    // only in how the sampler decodes them, and are distinct DXGI formats.
    BC1_RGBA_sRGB,
    BC2_RGBA_sRGB,
    BC3_RGBA_sRGB,
    BC4_RUnorm,
    BC4_RSnorm,
    BC5_RGUnorm,
    BC5_RGSnorm,
    BC6H_RGBUfloat,
    BC6H_RGBFloat,
    BC7_RGBAUnorm_sRGB,

    // Depth+Stencil combo
    D32Float_S8Uint,

    // Signed / packed formats used by D3D9 bump-maps and HDR (Learn D3DFORMAT).
    RG8Snorm,
    RGBA8Snorm,
    RGB10A2Unorm,
    /// DXGI_FORMAT_R11G11B10_FLOAT. Three unsigned floats packed into 32
    /// bits, no alpha. A D3D11 renderer uses it for HDR lookup tables and scene
    /// colour (Learn DXGI_FORMAT), and Metal's RG11B10Float is the exact match.
    RG11B10Float,

    // Integer formats. Learn DXGI_FORMAT: a _UINT/_SINT surface holds raw
    // integers, not normalized values, and a shader reads it through an
    // integer texture type. They are a distinct family from the _UNORM
    // formats sharing the same component layout, never an alias for one.
    R8Uint,
    R8Sint,
    RG8Uint,
    RG8Sint,
    RGBA8Uint,
    RGBA8Sint,
    R16Uint,
    R16Sint,
    RG16Uint,
    RG16Sint,
    RGBA16Uint,
    RGBA16Sint,
    R32Uint,
    R32Sint,
    RG32Uint,
    RG32Sint,
    RGBA32Uint,
    RGBA32Sint,
    RGB10A2Uint,

    // 16-bit signed-normalized. Learn DXGI_FORMAT: R16_SNORM, R16G16_SNORM
    // and R16G16B16A16_SNORM are typed members of the families whose UNORM,
    // FLOAT and integer forms are already host formats. Metal names them
    // R16Snorm (22), RG16Snorm (62) and RGBA16Snorm (112).
    R16Snorm,
    RG16Snorm,
    RGBA16Snorm,

    // Packed 16-bit and shared-exponent colour formats (MTLPixelFormat.h:
    // B5G6R5Unorm = 40, ABGR4Unorm = 42, BGR5A1Unorm = 43, RGB9E5Float = 93).
    // Measured on an Apple GPU (2026-09-28 contract audit, area 08, with
    // MTLTexture.replace + read): B5G6R5Unorm and BGR5A1Unorm store their
    // components at exactly the bit positions of DXGI B5G6R5_UNORM and
    // B5G5R5A1_UNORM (B in bits 0-4), and RGB9E5Float decodes the D3D11.3
    // spec example 0x999320c8 to (6.25, 12.5, 3.125). ABGR4Unorm holds A in
    // bits 0-3 .. R in 12-15, so DXGI B4G4R4A4_UNORM (B in 0-3 .. A in 12-15)
    // is read through the view swizzle (G, B, A, R).
    B5G6R5Unorm,
    ABGR4Unorm,
    BGR5A1Unorm,
    RGB9E5Float,

    // Planar 4:2:0 video formats (Learn DXGI_FORMAT: NV12, P010, P016): a
    // luma plane of W x H and an interleaved chroma plane of W/2 x H/2, one
    // subresource holding both. The backend keeps each plane as a texture of
    // its own (planar_chroma_format); format_table describes the luma plane.
    // P016 is stored as P010: "From the runtime's point of view,
    // DXGI_FORMAT_P010 is no different than DXGI_FORMAT_P016".
    NV12,
    P010,

    Count,
};

// =============================================================================
// Format Info
// =============================================================================

struct FormatInfo {
    u32 mtl_pixel_format;   // Raw MTLPixelFormat integer value
    u8  bytes_per_pixel;    // 0 for compressed (block size varies)
    u8  component_count;
    bool is_compressed;
    bool is_depth;
    bool is_srgb;
};

// MTLPixelFormat raw integer values (macOS / Apple Silicon):
//   A8Unorm             = 1       R8Unorm              = 10
//   R8Snorm             = 12
//   R16Unorm            = 20      R16Float             = 25
//   RG8Unorm            = 30      RG16Unorm            = 60
//   RG16Float           = 65      R32Float             = 55
//   RG32Float           = 105     RGBA8Unorm           = 70
//   RGBA8Unorm_sRGB     = 71      BGRA8Unorm           = 80
//   BGRA8Unorm_sRGB     = 81      RGBA16Unorm          = 110
//   RGBA16Float         = 115     RGBA32Float          = 125
//   BC1_RGBA            = 130     BC2_RGBA             = 132
//   BC3_RGBA            = 134     BC7_RGBAUnorm        = 152
//   Depth16Unorm        = 250     Depth32Float         = 252
//   Stencil8            = 253     Depth24Unorm_Stencil8= 255
//   Depth32Float_Stencil8 = 260     X32_Stencil8         = 261
//   X24_Stencil8          = 262
//
// Apple-family GPUs do not implement Depth24Unorm_Stencil8:
// -[MTLDevice depth24Stencil8PixelFormatSupported] is NO on every Apple
// Silicon device, and handing 255 to a texture descriptor is a hard Metal
// validation abort, not a soft failure. Every 24-bit depth format from every
// guest API therefore resolves to Depth32Float_Stencil8 (or Depth32Float when
// the guest format carries no stencil). Depth32Float strictly exceeds 24-bit
// unorm precision and the stencil plane is exact, so this substitution never
// loses guest-visible state.

inline constexpr FormatInfo format_table(PixelFormat fmt) {
    switch (fmt) {
    // 8-bit
    case PixelFormat::A8Unorm:          return {  1,  1, 1, false, false, false };
    case PixelFormat::R8Unorm:          return { 10,  1, 1, false, false, false };
    case PixelFormat::R8Snorm:          return { 12,  1, 1, false, false, false };

    // 16-bit
    case PixelFormat::R16Unorm:         return { 20,  2, 1, false, false, false };
    case PixelFormat::R16Float:         return { 25,  2, 1, false, false, false };
    case PixelFormat::RG8Unorm:         return { 30,  2, 2, false, false, false };
    case PixelFormat::D16Unorm:         return { 250, 2, 1, false, true,  false };

    // 32-bit
    case PixelFormat::R32Float:         return { 55,  4, 1, false, false, false };
    case PixelFormat::RG16Unorm:        return { 60,  4, 2, false, false, false };
    case PixelFormat::RG16Float:        return { 65,  4, 2, false, false, false };
    case PixelFormat::RGBA8Unorm:       return { 70,  4, 4, false, false, false };
    case PixelFormat::RGBA8Unorm_sRGB:  return { 71,  4, 4, false, false, true  };
    case PixelFormat::BGRA8Unorm:       return { 80,  4, 4, false, false, false };
    case PixelFormat::BGRA8Unorm_sRGB:  return { 81,  4, 4, false, false, true  };
    case PixelFormat::D24Unorm_S8Uint:  return { 260, 5, 2, false, true,  false };
    case PixelFormat::D32Float:         return { 252, 4, 1, false, true,  false };
    case PixelFormat::Stencil8:         return { 253, 1, 1, false, false, false };
    case PixelFormat::X32_Stencil8:     return { 261, 8, 1, false, false, false };
    case PixelFormat::X24_Stencil8:     return { 262, 4, 1, false, false, false };

    // 64-bit
    case PixelFormat::RG32Float:        return { 105, 8, 2, false, false, false };
    case PixelFormat::RGBA16Unorm:      return { 110, 8, 4, false, false, false };
    case PixelFormat::RGBA16Float:      return { 115, 8, 4, false, false, false };

    // 128-bit
    case PixelFormat::RGBA32Float:      return { 125, 16, 4, false, false, false };

    // Compressed
    case PixelFormat::BC1_RGBA:         return { 130, 0, 4, true, false, false };
    case PixelFormat::BC2_RGBA:         return { 132, 0, 4, true, false, false };
    case PixelFormat::BC3_RGBA:         return { 134, 0, 4, true, false, false };
    case PixelFormat::BC7_RGBAUnorm:    return { 152, 0, 4, true, false, false };
    case PixelFormat::BC1_RGBA_sRGB:    return { 131, 0, 4, true, false, true  };
    case PixelFormat::BC2_RGBA_sRGB:    return { 133, 0, 4, true, false, true  };
    case PixelFormat::BC3_RGBA_sRGB:    return { 135, 0, 4, true, false, true  };
    case PixelFormat::BC4_RUnorm:       return { 140, 0, 1, true, false, false };
    case PixelFormat::BC4_RSnorm:       return { 141, 0, 1, true, false, false };
    case PixelFormat::BC5_RGUnorm:      return { 142, 0, 2, true, false, false };
    case PixelFormat::BC5_RGSnorm:      return { 143, 0, 2, true, false, false };
    case PixelFormat::BC6H_RGBUfloat:   return { 151, 0, 3, true, false, false };
    case PixelFormat::BC6H_RGBFloat:    return { 150, 0, 3, true, false, false };
    case PixelFormat::BC7_RGBAUnorm_sRGB: return { 153, 0, 4, true, false, true  };

    // Depth+Stencil
    case PixelFormat::D32Float_S8Uint:  return { 260, 5, 2, false, true, false };

    case PixelFormat::RG8Snorm:         return { 32,  2, 2, false, false, false };
    // MTLPixelFormatRGBA8Snorm is 72; 73 is RGBA8Uint, a different family.
    case PixelFormat::RGBA8Snorm:       return { 72,  4, 4, false, false, false };
    case PixelFormat::RGB10A2Unorm:     return { 90,  4, 4, false, false, false };
    case PixelFormat::RG11B10Float:     return { 92,  4, 3, false, false, false };

    // Integer formats (MTLPixelFormat values from Metal's MTLPixelFormat.h).
    case PixelFormat::R8Uint:           return { 13,  1, 1, false, false, false };
    case PixelFormat::R8Sint:           return { 14,  1, 1, false, false, false };
    case PixelFormat::RG8Uint:          return { 33,  2, 2, false, false, false };
    case PixelFormat::RG8Sint:          return { 34,  2, 2, false, false, false };
    case PixelFormat::RGBA8Uint:        return { 73,  4, 4, false, false, false };
    case PixelFormat::RGBA8Sint:        return { 74,  4, 4, false, false, false };
    case PixelFormat::R16Uint:          return { 23,  2, 1, false, false, false };
    case PixelFormat::R16Sint:          return { 24,  2, 1, false, false, false };
    case PixelFormat::RG16Uint:         return { 63,  4, 2, false, false, false };
    case PixelFormat::RG16Sint:         return { 64,  4, 2, false, false, false };
    case PixelFormat::RGBA16Uint:       return { 113, 8, 4, false, false, false };
    case PixelFormat::RGBA16Sint:       return { 114, 8, 4, false, false, false };
    case PixelFormat::R32Uint:          return { 53,  4, 1, false, false, false };
    case PixelFormat::R32Sint:          return { 54,  4, 1, false, false, false };
    case PixelFormat::RG32Uint:         return { 103, 8, 2, false, false, false };
    case PixelFormat::RG32Sint:         return { 104, 8, 2, false, false, false };
    case PixelFormat::RGBA32Uint:       return { 123, 16, 4, false, false, false };
    case PixelFormat::RGBA32Sint:       return { 124, 16, 4, false, false, false };
    case PixelFormat::RGB10A2Uint:      return { 91,  4, 4, false, false, false };

    // MTLPixelFormat.h: R16Snorm = 22, RG16Snorm = 62, RGBA16Snorm = 112.
    case PixelFormat::R16Snorm:         return { 22,  2, 1, false, false, false };
    case PixelFormat::RG16Snorm:        return { 62,  4, 2, false, false, false };
    case PixelFormat::RGBA16Snorm:      return { 112, 8, 4, false, false, false };

    case PixelFormat::B5G6R5Unorm:      return { 40,  2, 3, false, false, false };
    case PixelFormat::ABGR4Unorm:       return { 42,  2, 4, false, false, false };
    case PixelFormat::BGR5A1Unorm:      return { 43,  2, 4, false, false, false };
    case PixelFormat::RGB9E5Float:      return { 93,  4, 3, false, false, false };

    // Planar video formats: the luma plane (R8Unorm / R16Unorm).
    case PixelFormat::NV12:             return { 10,  1, 1, false, false, false };
    case PixelFormat::P010:             return { 20,  2, 1, false, false, false };

    default:                            return { 0, 0, 0, false, false, false };
    }
}

/// True for the _UINT / _SINT families. Learn DXGI_FORMAT: these surfaces are
/// read through an integer texture type and are never filtered, so a caller
/// that binds one has to select an integer sampler-free path rather than the
/// normalized float path used by every other colour format.
inline constexpr bool is_integer_format(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::R8Uint:
    case PixelFormat::R8Sint:
    case PixelFormat::RG8Uint:
    case PixelFormat::RG8Sint:
    case PixelFormat::RGBA8Uint:
    case PixelFormat::RGBA8Sint:
    case PixelFormat::R16Uint:
    case PixelFormat::R16Sint:
    case PixelFormat::RG16Uint:
    case PixelFormat::RG16Sint:
    case PixelFormat::RGBA16Uint:
    case PixelFormat::RGBA16Sint:
    case PixelFormat::R32Uint:
    case PixelFormat::R32Sint:
    case PixelFormat::RG32Uint:
    case PixelFormat::RG32Sint:
    case PixelFormat::RGBA32Uint:
    case PixelFormat::RGBA32Sint:
    case PixelFormat::RGB10A2Uint:
        return true;
    default:
        return false;
    }
}

// =============================================================================
// DXGI_FORMAT -> PixelFormat
// =============================================================================

inline constexpr PixelFormat pixel_format_from_dxgi(u32 dxgi) {
    switch (dxgi) {
    // Typeless DXGI formats allocate typed storage for the family default
    // (Learn CreateTexture2D + Create*View reinterpret). Guest desc.format
    // remains typeless; only the host pixel format is chosen here.
    case 1:  return PixelFormat::RGBA32Float;   // R32G32B32A32_TYPELESS
    case 2:  return PixelFormat::RGBA32Float;
    case 9:  return PixelFormat::RGBA16Float;   // R16G16B16A16_TYPELESS
    case 10: return PixelFormat::RGBA16Float;
    case 11: return PixelFormat::RGBA16Unorm;
    case 15: return PixelFormat::RG32Float;     // R32G32_TYPELESS
    case 16: return PixelFormat::RG32Float;
    case 27: return PixelFormat::RGBA8Unorm;    // R8G8B8A8_TYPELESS
    case 28: return PixelFormat::RGBA8Unorm;
    case 29: return PixelFormat::RGBA8Unorm_sRGB;
    case 33: return PixelFormat::RG16Float;     // R16G16_TYPELESS
    case 34: return PixelFormat::RG16Float;
    case 35: return PixelFormat::RG16Unorm;
    case 39: return PixelFormat::R32Float;      // R32_TYPELESS
    case 40: return PixelFormat::D32Float;
    case 41: return PixelFormat::R32Float;
    // Combined depth+stencil families. These were missing, so a guest could not
    // create the standard sampleable depth buffer at all -- an R24G8_TYPELESS
    // resource with a D24_UNORM_S8_UINT DSV and an R24_UNORM_X8_TYPELESS SRV --
    // nor a staging texture of one. Found by the Windows differential
    // (2026-09-22), where R24G8_TYPELESS staging succeeds and Vitreo refused it.
    // The X8/G8 members name one plane of the pair; the resource still allocates
    // the combined format, and the view selects the plane.
    case 19: return PixelFormat::D32Float_S8Uint;  // R32G8X24_TYPELESS
    case 21: return PixelFormat::D32Float_S8Uint;  // R32_FLOAT_X8X24_TYPELESS
    case 22: return PixelFormat::D32Float_S8Uint;  // X32_TYPELESS_G8X24_UINT
    case 44: return PixelFormat::D24Unorm_S8Uint;  // R24G8_TYPELESS
    case 45: return PixelFormat::D24Unorm_S8Uint;
    case 46: return PixelFormat::D24Unorm_S8Uint;  // R24_UNORM_X8_TYPELESS
    case 47: return PixelFormat::D24Unorm_S8Uint;  // X24_TYPELESS_G8_UINT
    case 48: return PixelFormat::RG8Unorm;      // R8G8_TYPELESS
    case 49: return PixelFormat::RG8Unorm;
    case 53: return PixelFormat::R16Unorm;      // R16_TYPELESS
    case 54: return PixelFormat::R16Float;
    case 55: return PixelFormat::D16Unorm;
    case 56: return PixelFormat::R16Unorm;
    case 60: return PixelFormat::R8Unorm;       // R8_TYPELESS
    case 61: return PixelFormat::R8Unorm;
    case 63: return PixelFormat::R8Snorm;
    case 65: return PixelFormat::A8Unorm;
    case 70: return PixelFormat::BC1_RGBA;       // BC1_TYPELESS
    case 71: return PixelFormat::BC1_RGBA;
    case 72: return PixelFormat::BC1_RGBA_sRGB;
    case 73: return PixelFormat::BC2_RGBA;       // BC2_TYPELESS
    case 74: return PixelFormat::BC2_RGBA;
    case 75: return PixelFormat::BC2_RGBA_sRGB;
    case 76: return PixelFormat::BC3_RGBA;       // BC3_TYPELESS
    case 77: return PixelFormat::BC3_RGBA;
    case 78: return PixelFormat::BC3_RGBA_sRGB;
    case 79: return PixelFormat::BC4_RUnorm;     // BC4_TYPELESS
    case 80: return PixelFormat::BC4_RUnorm;
    case 81: return PixelFormat::BC4_RSnorm;
    case 82: return PixelFormat::BC5_RGUnorm;    // BC5_TYPELESS
    case 83: return PixelFormat::BC5_RGUnorm;
    case 84: return PixelFormat::BC5_RGSnorm;
    case 94: return PixelFormat::BC6H_RGBUfloat; // BC6H_TYPELESS
    case 95: return PixelFormat::BC6H_RGBUfloat;
    case 96: return PixelFormat::BC6H_RGBFloat;
    case 97: return PixelFormat::BC7_RGBAUnorm;  // BC7_TYPELESS
    case 99: return PixelFormat::BC7_RGBAUnorm_sRGB;
    case 87: return PixelFormat::BGRA8Unorm;
    case 90: return PixelFormat::BGRA8Unorm;    // B8G8R8A8_TYPELESS
    case 91: return PixelFormat::BGRA8Unorm_sRGB;
    case 98: return PixelFormat::BC7_RGBAUnorm;
    case 20: return PixelFormat::D32Float_S8Uint;
    // Packed and signed-normalized colour formats whose host equivalents
    // already existed but were unreachable from a DXGI desc.
    case 23: return PixelFormat::RGB10A2Unorm;  // R10G10B10A2_TYPELESS
    case 24: return PixelFormat::RGB10A2Unorm;
    case 26: return PixelFormat::RG11B10Float;  // R11G11B10_FLOAT
    case 31: return PixelFormat::RGBA8Snorm;
    case 51: return PixelFormat::RG8Snorm;
    // Integer families.
    case 3:  return PixelFormat::RGBA32Uint;
    case 4:  return PixelFormat::RGBA32Sint;
    case 12: return PixelFormat::RGBA16Uint;
    case 13: return PixelFormat::RGBA16Snorm;  // R16G16B16A16_SNORM
    case 14: return PixelFormat::RGBA16Sint;
    case 17: return PixelFormat::RG32Uint;
    case 18: return PixelFormat::RG32Sint;
    case 25: return PixelFormat::RGB10A2Uint;
    case 30: return PixelFormat::RGBA8Uint;
    case 32: return PixelFormat::RGBA8Sint;
    case 36: return PixelFormat::RG16Uint;
    case 37: return PixelFormat::RG16Snorm;    // R16G16_SNORM
    case 38: return PixelFormat::RG16Sint;
    case 42: return PixelFormat::R32Uint;
    case 43: return PixelFormat::R32Sint;
    case 50: return PixelFormat::RG8Uint;
    case 52: return PixelFormat::RG8Sint;
    case 57: return PixelFormat::R16Uint;
    case 58: return PixelFormat::R16Snorm;     // R16_SNORM
    case 59: return PixelFormat::R16Sint;
    case 62: return PixelFormat::R8Uint;
    case 64: return PixelFormat::R8Sint;
    // Formats the Learn FL 11_0 table requires that have an exact Metal
    // storage format (see the PixelFormat entries above). B8G8R8X8 is BGRA8
    // storage whose fourth byte is not a component: shader views read it
    // through an alpha = ONE swizzle and render targets never write it.
    case 67: return PixelFormat::RGB9E5Float;   // R9G9B9E5_SHAREDEXP
    case 85: return PixelFormat::B5G6R5Unorm;
    case 86: return PixelFormat::BGR5A1Unorm;   // B5G5R5A1_UNORM
    case 88: return PixelFormat::BGRA8Unorm;    // B8G8R8X8_UNORM
    case 92: return PixelFormat::BGRA8Unorm;    // B8G8R8X8_TYPELESS
    case 93: return PixelFormat::BGRA8Unorm_sRGB;  // B8G8R8X8_UNORM_SRGB
    case 115: return PixelFormat::ABGR4Unorm;   // B4G4R4A4_UNORM
    case 103: return PixelFormat::NV12;
    case 104: return PixelFormat::P010;
    case 105: return PixelFormat::P010;         // P016: P010 storage
    // Learn DXGI_FORMAT, "Direct3D 11 video resource formats": the packed
    // video formats are stored as the one view format family Learn gives
    // each -- one texel of AYUV is an R8G8B8A8 texel (V, U, Y, A), of Y410
    // an R10G10B10A2 one, of Y416 an R16G16B16A16 one -- and the packed
    // 4:2:2 formats as that texel holding a pair of pixels (Y0, U, Y1, V),
    // so their storage is half as wide as the resource (the D3D11 frontend
    // converts; dxgi_pixels_per_texel). 420_OPAQUE is an NV12-shaped planar
    // resource nothing but copies reach; P8 / A8P8 are opaque 8 / 16-bit
    // texels no shader reads. None of them is a view format: the frontend
    // validates every view against the resource's Learn view list first.
    case 100: return PixelFormat::RGBA8Unorm;   // AYUV
    case 101: return PixelFormat::RGB10A2Unorm; // Y410
    case 102: return PixelFormat::RGBA16Unorm;  // Y416
    case 106: return PixelFormat::NV12;         // 420_OPAQUE
    case 107: return PixelFormat::RGBA8Unorm;   // YUY2 (two pixels a texel)
    case 108: return PixelFormat::RGBA16Unorm;  // Y210 (two pixels a texel)
    case 109: return PixelFormat::RGBA16Unorm;  // Y216 (two pixels a texel)
    case 113: return PixelFormat::R8Unorm;      // P8
    case 114: return PixelFormat::RG8Unorm;     // A8P8
    default: return PixelFormat::Unknown;
    }
}

/// The chroma-plane format of a planar video format (Learn DXGI_FORMAT: the
/// chrominance views are R8G8 for NV12 and R16G16 for P010 / P016), or
/// Unknown for every single-plane format.
inline constexpr PixelFormat planar_chroma_format(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::NV12: return PixelFormat::RG8Unorm;
    case PixelFormat::P010: return PixelFormat::RG16Unorm;
    default: return PixelFormat::Unknown;
    }
}

inline constexpr bool is_planar_format(PixelFormat fmt) {
    return planar_chroma_format(fmt) != PixelFormat::Unknown;
}

// =============================================================================
// Compressed (BC) helpers
// =============================================================================

/// Bytes in one BC block for a Metal pixel format; 0 if not a BC format.
/// Block footprint is always 4×4 texels for the BC family Metal exposes.
inline constexpr u32 compressed_block_bytes_mtl(u32 mtl_format) noexcept {
    switch (mtl_format) {
    case 130: // BC1_RGBA
    case 131: // BC1_RGBA_sRGB
    case 140: // BC4_RUnorm
    case 141: // BC4_RSnorm
        return 8;
    case 132: // BC2_RGBA
    case 133: // BC2_RGBA_sRGB
    case 134: // BC3_RGBA
    case 135: // BC3_RGBA_sRGB
    case 142: // BC5_RGUnorm
    case 143: // BC5_RGSnorm
    case 150: // BC6H_RGBFloat
    case 151: // BC6H_RGBUfloat
    case 152: // BC7_RGBAUnorm
    case 153: // BC7_RGBAUnorm_sRGB
        return 16;
    default:
        return 0;
    }
}

inline constexpr u32 compressed_block_bytes(PixelFormat fmt) noexcept {
    return compressed_block_bytes_mtl(format_table(fmt).mtl_pixel_format);
}

// =============================================================================
// Bytes Per Pixel (non-compressed MTL formats)
// =============================================================================

inline constexpr u32 bytes_per_pixel(u32 mtl_format) {
    switch (mtl_format) {
    // 1-byte formats
    case 10:  return 1;   // R8Unorm
    case 12:  return 1;   // R8Snorm
    case 13:  return 1;   // R8Uint
    case 14:  return 1;   // R8Sint
    case 253: return 1;   // Stencil8

    // 2-byte formats
    case 20:  return 2;   // R16Unorm
    case 21:  return 2;   // R16Snorm
    case 22:  return 2;   // R16Uint
    case 23:  return 2;   // R16Sint
    case 25:  return 2;   // R16Float
    case 30:  return 2;   // RG8Unorm
    case 31:  return 2;   // RG8Uint
    case 32:  return 2;   // RG8Snorm
    case 33:  return 2;   // RG8Sint
    case 250: return 2;   // Depth16Unorm

    // 4-byte formats
    case 53:  return 4;   // R32Uint
    case 54:  return 4;   // R32Sint
    case 55:  return 4;   // R32Float
    case 60:  return 4;   // RG16Unorm
    case 61:  return 4;   // RG16Snorm
    case 62:  return 4;   // RG16Uint
    case 63:  return 4;   // RG16Sint
    case 65:  return 4;   // RG16Float
    case 70:  return 4;   // RGBA8Unorm
    case 71:  return 4;   // RGBA8Unorm_sRGB
    case 72:  return 4;   // RGBA8Uint
    case 73:  return 4;   // RGBA8Snorm
    case 74:  return 4;   // RGBA8Sint
    case 80:  return 4;   // BGRA8Unorm
    case 81:  return 4;   // BGRA8Unorm_sRGB
    case 90:  return 4;   // RGB10A2Unorm
    case 92:  return 4;   // RG11B10Float
    case 252: return 4;   // Depth32Float

    // 8-byte formats
    case 105: return 8;   // RG32Float
    case 106: return 8;   // RG32Uint
    case 107: return 8;   // RG32Sint
    case 110: return 8;   // RGBA16Unorm
    case 111: return 8;   // RGBA16Snorm
    case 112: return 8;   // RGBA16Uint
    case 113: return 8;   // RGBA16Sint
    case 115: return 8;   // RGBA16Float

    // 16-byte formats
    case 125: return 16;  // RGBA32Float
    case 126: return 16;  // RGBA32Uint
    case 127: return 16;  // RGBA32Sint

    // 5-byte (depth + stencil)
    case 260: return 5;   // Depth32Float_Stencil8

    default:  return 0;   // Compressed or unknown
    }
}

} // namespace vitreo::graphics
