// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_format_caps.cpp
/// @brief Which resource each D3DFORMAT can be: the one answer
///        IDirect3D9::CheckDeviceFormat and every Create* share.
///
/// A runtime whose Create* disagrees with its own CheckDeviceFormat breaks
/// applications that pick formats from the caps (Microsoft Learn,
/// IDirect3D9::CheckDeviceFormat). The table is the RTX 4080's
/// CheckDeviceFormat answer (tools/oracle_probe/d3d9_format_probe32
/// .winoracle-gpu.expected), kept only where Vitreo's backend creates the
/// resource, plus formats Vitreo converts on upload that the driver no longer
/// offers (R8G8B8, X4R4G4B4, A8B8G8R8, X8B8G8R8, P8, A8P8, CxV8U8), for plain
/// textures and offscreen surfaces only. Formats the driver offers that
/// Vitreo cannot create yet (INTZ, NULL, ATI1, ATI2, A2R10G10B10, YUY2/UYVY
/// surfaces) answer D3DERR_NOTAVAILABLE; see .docs/contracts/d3d9.md.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

namespace vitreo::winapi {

namespace {

struct FormatCaps {
    u32 format;
    bool texture;
    bool cube_texture;
    bool volume_texture;
    bool render_target;
    bool offscreen_plain;
    bool depth_stencil;
    bool depth_texture;
    bool autogen_mipmap;
};

constexpr FormatCaps kFormatCaps[] = {
    {20 /* R8G8B8 */, true, true, true, false, true, false, false, false},
    {21 /* A8R8G8B8 */, true, true, true, true, true, false, false, true},
    {22 /* X8R8G8B8 */, true, true, true, true, true, false, false, true},
    {23 /* R5G6B5 */, true, true, true, true, true, false, false, true},
    {24 /* X1R5G5B5 */, true, true, true, true, true, false, false, true},
    // Render target: a Vitreo extra -- GetRenderTargetData packs it exactly.
    {25 /* A1R5G5B5 */, true, true, true, true, true, false, false, false},
    {26 /* A4R4G4B4 */, true, true, true, false, true, false, false, false},
    {28 /* A8 */, true, true, true, true, false, false, false, false},
    {30 /* X4R4G4B4 */, true, true, true, false, true, false, false, false},
    {31 /* A2B10G10R10 */, true, true, true, true, true, false, false, true},
    {32 /* A8B8G8R8 */, true, true, true, false, true, false, false, false},
    {33 /* X8B8G8R8 */, true, true, true, false, true, false, false, false},
    {34 /* G16R16 */, true, true, true, true, true, false, false, true},
    {36 /* A16B16G16R16 */, true, true, true, true, true, false, false, true},
    {40 /* A8P8 */, true, true, true, false, true, false, false, false},
    {41 /* P8 */, true, true, true, false, true, false, false, false},
    {50 /* L8 */, true, true, true, true, false, false, false, true},
    {51 /* A8L8 */, true, true, true, false, false, false, false, false},
    {60 /* V8U8 */, true, false, false, false, false, false, false, false},
    {62 /* X8L8V8U8 */, true, false, false, false, false, false, false, false},
    {63 /* Q8W8V8U8 */, true, true, false, false, false, false, false, false},
    {64 /* V16U16 */, true, true, true, false, false, false, false, false},
    {0x31545844 /* DXT1 */, true, true, true, false, false, false, false, false},
    {0x32545844 /* DXT2 */, true, true, true, false, false, false, false, false},
    {0x33545844 /* DXT3 */, true, true, true, false, false, false, false, false},
    {0x34545844 /* DXT4 */, true, true, true, false, false, false, false, false},
    {0x35545844 /* DXT5 */, true, true, true, false, false, false, false, false},
    {75 /* D24S8 */, false, false, false, false, false, true, true, false},
    {77 /* D24X8 */, false, false, false, false, false, true, true, false},
    {80 /* D16 */, false, false, false, false, false, true, true, false},
    {82 /* D32F_LOCKABLE */, false, false, false, false, false, true, true, false},
    {81 /* L16 */, true, true, true, true, false, false, false, true},
    {111 /* R16F */, true, true, true, true, true, false, false, true},
    {112 /* G16R16F */, true, true, true, true, true, false, false, true},
    {113 /* A16B16G16R16F */, true, true, true, true, true, false, false, true},
    {114 /* R32F */, true, true, true, true, true, false, false, true},
    {115 /* G32R32F */, true, true, true, true, true, false, false, true},
    {116 /* A32B32G32R32F */, true, true, true, true, true, false, false, true},
    {117 /* CxV8U8 */, true, true, true, false, true, false, false, false},
};

constexpr u32 kUsageRenderTarget = 0x00000001u;
constexpr u32 kUsageDepthStencil = 0x00000002u;
constexpr u32 kUsageAutoGenMipMap = 0x00000400u;

} // namespace

u32 d3d9_check_format_usage(u32 format, u32 resource_type, u32 usage) {
    const FormatCaps* caps = nullptr;
    for (const FormatCaps& entry : kFormatCaps) {
        if (entry.format == format) {
            caps = &entry;
            break;
        }
    }
    if (caps == nullptr) return D3DERR_NOTAVAILABLE;
    const bool render_target = (usage & kUsageRenderTarget) != 0;
    const bool depth_stencil = (usage & kUsageDepthStencil) != 0;
    bool supported = false;
    bool mipmapped = false;
    switch (resource_type) {
    case D3DRTYPE_SURFACE:
        supported = depth_stencil   ? caps->depth_stencil
                    : render_target ? caps->render_target
                                    : caps->offscreen_plain;
        break;
    case D3DRTYPE_TEXTURE:
        supported = depth_stencil   ? caps->depth_texture
                    : render_target ? caps->render_target && caps->texture
                                    : caps->texture;
        mipmapped = !depth_stencil;
        break;
    case D3DRTYPE_CUBETEXTURE:
        supported = !depth_stencil &&
                    (render_target ? caps->render_target && caps->cube_texture
                                   : caps->cube_texture);
        mipmapped = true;
        break;
    case D3DRTYPE_VOLUMETEXTURE:
    case D3DRTYPE_VOLUME:
        supported = !depth_stencil && !render_target && caps->volume_texture;
        break;
    default:
        break;
    }
    if (!supported) return D3DERR_NOTAVAILABLE;
    // Learn: D3DOK_NOAUTOGEN -- the format is usable but mipmaps will not be
    // generated automatically for it (measured for 21 formats).
    if ((usage & kUsageAutoGenMipMap) != 0 && mipmapped &&
        !caps->autogen_mipmap) {
        return D3DOK_NOAUTOGEN;
    }
    return D3D_OK;
}

} // namespace vitreo::winapi
