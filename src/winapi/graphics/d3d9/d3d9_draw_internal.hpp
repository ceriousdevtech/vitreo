// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d9_draw_internal.hpp
/// @brief Private surface shared by the d3d9_draw*.cpp parts and
///        d3d9_process_vertices.cpp.
///
/// Private to the D3D9 frontend. These were file-local helpers in an anonymous
/// namespace at the top of d3d9_draw.cpp before that file was split by
/// responsibility; they are here so the parts can still see them, not to
/// publish them. Each consumer writes `using namespace d3d9_draw_detail;`
/// inside `namespace vitreo::winapi`, which gives the same lookup the
/// anonymous namespace gave.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "graphics/shaders/ff_uniforms.hpp"

#include <array>
#include <vector>

namespace vitreo::winapi::d3d9_draw_detail {

// D3DPRIMITIVETYPE
inline constexpr u32 D3DPT_POINTLIST = 1;
inline constexpr u32 D3DPT_LINELIST = 2;
inline constexpr u32 D3DPT_LINESTRIP = 3;
inline constexpr u32 D3DPT_TRIANGLELIST = 4;
inline constexpr u32 D3DPT_TRIANGLESTRIP = 5;
inline constexpr u32 D3DPT_TRIANGLEFAN = 6;

// Render states the draw path reads.
inline constexpr u32 D3DRS_ZENABLE = 7;
inline constexpr u32 D3DRS_SHADEMODE = 9;
inline constexpr u32 D3DRS_ZWRITEENABLE = 14;
inline constexpr u32 D3DRS_ALPHATESTENABLE = 15;
inline constexpr u32 D3DRS_SRCBLEND = 19;
inline constexpr u32 D3DRS_DESTBLEND = 20;
inline constexpr u32 D3DRS_CULLMODE = 22;
inline constexpr u32 D3DRS_ZFUNC = 23;
inline constexpr u32 D3DRS_ALPHAREF = 24;
inline constexpr u32 D3DRS_ALPHAFUNC = 25;
inline constexpr u32 D3DRS_ALPHABLENDENABLE = 27;
inline constexpr u32 D3DRS_FOGENABLE = 28;
inline constexpr u32 D3DRS_SPECULARENABLE = 29;
inline constexpr u32 D3DRS_FOGCOLOR = 34;
inline constexpr u32 D3DRS_FOGTABLEMODE = 35;
inline constexpr u32 D3DRS_FOGSTART = 36;
inline constexpr u32 D3DRS_FOGEND = 37;
inline constexpr u32 D3DRS_FOGDENSITY = 38;
inline constexpr u32 D3DRS_RANGEFOGENABLE = 48;
inline constexpr u32 D3DRS_AMBIENT = 139;
inline constexpr u32 D3DRS_COLORVERTEX = 141;
inline constexpr u32 D3DRS_LOCALVIEWER = 142;
inline constexpr u32 D3DRS_NORMALIZENORMALS = 143;
inline constexpr u32 D3DRS_POINTSIZE = 154;
inline constexpr u32 D3DRS_POINTSIZE_MIN = 155;
inline constexpr u32 D3DRS_POINTSCALEENABLE = 157;
inline constexpr u32 D3DRS_POINTSCALE_A = 158;
inline constexpr u32 D3DRS_POINTSCALE_B = 159;
inline constexpr u32 D3DRS_POINTSCALE_C = 160;
inline constexpr u32 D3DRS_POINTSIZE_MAX = 166;
inline constexpr u32 D3DRS_AMBIENTMATERIALSOURCE = 147;
inline constexpr u32 D3DRS_DIFFUSEMATERIALSOURCE = 145;
inline constexpr u32 D3DRS_SPECULARMATERIALSOURCE = 146;
inline constexpr u32 D3DRS_EMISSIVEMATERIALSOURCE = 148;
inline constexpr u32 D3DRS_FOGVERTEXMODE = 140;
inline constexpr u32 D3DRS_LIGHTING = 137;
inline constexpr u32 D3DRS_COLORWRITEENABLE = 168;
inline constexpr u32 D3DRS_BLENDOP = 171;
inline constexpr u32 D3DRS_COLORWRITEENABLE1 = 190;
inline constexpr u32 D3DRS_COLORWRITEENABLE2 = 191;
inline constexpr u32 D3DRS_COLORWRITEENABLE3 = 192;
inline constexpr u32 D3DRS_BLENDFACTOR = 193;
inline constexpr u32 D3DRS_SEPARATEALPHABLENDENABLE = 206;
inline constexpr u32 D3DRS_SRCBLENDALPHA = 207;
inline constexpr u32 D3DRS_DESTBLENDALPHA = 208;
inline constexpr u32 D3DRS_BLENDOPALPHA = 209;
inline constexpr u32 D3DRS_SCISSORTESTENABLE = 174;
inline constexpr u32 D3DRS_TEXTUREFACTOR = 60;
inline constexpr u32 D3DRS_CLIPPING = 136;
inline constexpr u32 D3DRS_CLIPPLANEENABLE = 152;
inline constexpr u32 D3DRS_SLOPESCALEDEPTHBIAS = 175;
inline constexpr u32 D3DRS_DEPTHBIAS = 195;
inline constexpr u32 D3DRS_SRGBWRITEENABLE = 194;

// Texture stage / sampler state indices.
inline constexpr u32 D3DTSS_COLOROP = 1;
inline constexpr u32 D3DTSS_COLORARG1 = 2;
inline constexpr u32 D3DTSS_COLORARG2 = 3;
inline constexpr u32 D3DTSS_ALPHAOP = 4;
inline constexpr u32 D3DTSS_ALPHAARG1 = 5;
inline constexpr u32 D3DTSS_ALPHAARG2 = 6;
inline constexpr u32 D3DTSS_BUMPENVMAT00 = 7;
inline constexpr u32 D3DTSS_BUMPENVLSCALE = 22;
inline constexpr u32 D3DTSS_BUMPENVLOFFSET = 23;
inline constexpr u32 D3DTSS_BUMPENVMAT01 = 8;
inline constexpr u32 D3DTSS_BUMPENVMAT10 = 9;
inline constexpr u32 D3DTSS_BUMPENVMAT11 = 10;
inline constexpr u32 D3DTSS_TEXCOORDINDEX = 11;
inline constexpr u32 D3DTSS_TEXTURETRANSFORMFLAGS = 24;
inline constexpr u32 D3DTSS_COLORARG0 = 26;
inline constexpr u32 D3DTSS_ALPHAARG0 = 27;
inline constexpr u32 D3DTSS_RESULTARG = 28;
inline constexpr u32 D3DTSS_CONSTANT = 32;
inline constexpr u32 D3DSAMP_ADDRESSU = 1;
inline constexpr u32 D3DSAMP_ADDRESSV = 2;
inline constexpr u32 D3DSAMP_ADDRESSW = 3;
inline constexpr u32 D3DSAMP_BORDERCOLOR = 4;
inline constexpr u32 D3DSAMP_MAGFILTER = 5;
inline constexpr u32 D3DSAMP_MINFILTER = 6;
inline constexpr u32 D3DSAMP_MIPFILTER = 7;
inline constexpr u32 D3DSAMP_MIPMAPLODBIAS = 8;
inline constexpr u32 D3DSAMP_MAXMIPLEVEL = 9;
inline constexpr u32 D3DSAMP_MAXANISOTROPY = 10;
inline constexpr u32 D3DSAMP_SRGBTEXTURE = 11;

// D3DTRANSFORMSTATETYPE
inline constexpr u32 D3DTS_VIEW = 2;
inline constexpr u32 D3DTS_PROJECTION = 3;
inline constexpr u32 D3DTS_TEXTURE0 = 16;
inline constexpr u32 D3DTS_WORLD = 256;

// D3DFVF position masks.
inline constexpr u32 D3DFVF_POSITION_MASK = 0x400E;
inline constexpr u32 D3DFVF_XYZ = 0x002;
inline constexpr u32 D3DFVF_XYZRHW = 0x004;
inline constexpr u32 D3DFVF_XYZB1 = 0x006;
inline constexpr u32 D3DFVF_XYZB2 = 0x008;
inline constexpr u32 D3DFVF_XYZB3 = 0x00A;
inline constexpr u32 D3DFVF_XYZB4 = 0x00C;
inline constexpr u32 D3DFVF_XYZB5 = 0x00E;
inline constexpr u32 D3DFVF_LASTBETA_UBYTE4 = 0x1000;
inline constexpr u32 D3DFVF_NORMAL = 0x010;
inline constexpr u32 D3DFVF_PSIZE = 0x020;
inline constexpr u32 D3DFVF_DIFFUSE = 0x040;
inline constexpr u32 D3DFVF_SPECULAR = 0x080;
inline constexpr u32 D3DFVF_TEXCOUNT_MASK = 0xF00;
inline constexpr u32 D3DFVF_TEXCOUNT_SHIFT = 8;

/// Matrix math is shared with the DirectX 7 fixed-function path; see
/// `graphics/shaders/ff_uniforms.hpp` for the row-major/row-vector contract.
using Matrix = graphics::FFMatrix;

inline constexpr Matrix IDENTITY = graphics::FF_IDENTITY_MATRIX;

/// Unpack a D3DCOLOR into the RGBA order the generated shaders expect.
[[nodiscard]] inline std::array<float, 4> color_to_rgba(u32 color) {
    return graphics::ff_color_to_rgba(color);
}

[[nodiscard]] inline const D3D9Manager::FvfElement* find_fvf_element(
    const std::vector<D3D9Manager::FvfElement>& elements, u32 usage,
    u32 usage_index) {
    for (const auto& element : elements) {
        if (element.usage == usage && element.usage_index == usage_index) {
            return &element;
        }
    }
    return nullptr;
}

/// Vertex count for a primitive count of a given type.
[[nodiscard]] inline u32 vertices_for_primitives(u32 primitive_type,
                                                 u32 count) {
    switch (primitive_type) {
    case D3DPT_POINTLIST:     return count;
    case D3DPT_LINELIST:      return count * 2;
    case D3DPT_LINESTRIP:     return count + 1;
    case D3DPT_TRIANGLELIST:  return count * 3;
    case D3DPT_TRIANGLESTRIP: return count + 2;
    case D3DPT_TRIANGLEFAN:   return count + 2;
    default: return 0;
    }
}

/// D3DSAMP_SRGBTEXTURE as a bitmask over the pixel-stage samplers.
[[nodiscard]] inline u32 d3d9_srgb_texture_mask(
    const D3d9FrozenDrawTables& tables) {
    u32 mask = 0;
    for (u32 sampler = 0; sampler < D3D9_MAX_SAMPLERS && sampler < 32u;
         ++sampler) {
        const auto& map = tables.sampler_states[sampler];
        const auto found = map.find(D3DSAMP_SRGBTEXTURE);
        const u32 value = found != map.end()
            ? found->second
            : d3d9_default_sampler_state(D3DSAMP_SRGBTEXTURE);
        if (value != 0) mask |= 1u << sampler;
    }
    return mask;
}

} // namespace vitreo::winapi::d3d9_draw_detail
