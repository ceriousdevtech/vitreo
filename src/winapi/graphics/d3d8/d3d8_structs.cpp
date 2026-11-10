// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d8_structs.cpp
/// @brief Translation of the D3D8 structures whose layout or meaning differs
///        from their D3D9 successors. Every offset is generated from the
///        SDK headers (d3d8_contract.inc).

#include "winapi/graphics/d3d8/d3d8_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

using namespace d3d8c;

namespace {
bool failed(u32 hr) { return (hr & 0x80000000u) != 0; }
}  // namespace

// -----------------------------------------------------------------------------
// D3DPRESENT_PARAMETERS
// -----------------------------------------------------------------------------
//
// Learn ("Converting to Direct3D 9", Intervals and SwapEffects): D3D9 removed
// D3DSWAPEFFECT_COPY_VSYNC and D3DPRESENT_RATE_UNLIMITED, renamed
// FullScreen_PresentationInterval to PresentationInterval, and added
// D3DPRESENT_INTERVAL_IMMEDIATE as the windowed "do not wait" interval;
// D3D9 also added MultiSampleQuality. A D3D8 windowed swap chain presents
// without waiting for the vertical blank unless its swap effect is
// COPY_VSYNC, so a windowed D3D8 chain is IMMEDIATE (or ONE for COPY_VSYNC),
// and a full-screen chain keeps its interval.

GuestAddr D3D8Manager::present_parameters_to9(GuestAddr pp8, u32& hr) {
    namespace L = layout_PRESENT_PARAMETERS;
    VITREO_TRACE("[d3d8] present parameters %ux%u fmt=%u count=%u ms=%u swap=%u "
                 "hwnd=0x%x windowed=%u autods=%u dsfmt=%u flags=0x%x refresh=%u "
                 "interval=0x%x\n",
                 rd(at(pp8, L::BackBufferWidth.d3d8)), rd(at(pp8, L::BackBufferHeight.d3d8)),
                 rd(at(pp8, L::BackBufferFormat.d3d8)), rd(at(pp8, L::BackBufferCount.d3d8)),
                 rd(at(pp8, L::MultiSampleType.d3d8)), rd(at(pp8, L::SwapEffect.d3d8)),
                 rd(at(pp8, L::hDeviceWindow.d3d8)), rd(at(pp8, L::Windowed.d3d8)),
                 rd(at(pp8, L::EnableAutoDepthStencil.d3d8)),
                 rd(at(pp8, L::AutoDepthStencilFormat.d3d8)), rd(at(pp8, L::Flags.d3d8)),
                 rd(at(pp8, L::FullScreen_RefreshRateInHz.d3d8)),
                 rd(at(pp8, L::FullScreen_PresentationInterval.d3d8)));
    const GuestAddr pp9 = scratch(L::SIZE9);
    if (pp9 == 0) {
        hr = d3d8::E_OUTOFMEMORY;
        return 0;
    }
    for (const FieldPair field :
         {L::BackBufferWidth, L::BackBufferHeight, L::BackBufferFormat,
          L::BackBufferCount, L::MultiSampleType, L::hDeviceWindow, L::Windowed,
          L::EnableAutoDepthStencil, L::AutoDepthStencilFormat, L::Flags,
          L::FullScreen_RefreshRateInHz}) {
        wr(at(pp9, field.d3d9), rd(at(pp8, field.d3d8)));
    }
    wr(at(pp9, L::MultiSampleQuality.d3d9), 0);
    const u32 swap = rd(at(pp8, L::SwapEffect.d3d8));
    const bool windowed = rd(at(pp8, L::Windowed.d3d8)) != 0;
    const u32 interval8 = rd(at(pp8, L::FullScreen_PresentationInterval.d3d8));
    const u32 rate = rd(at(pp8, L::FullScreen_RefreshRateInHz.d3d8));
    if (windowed && (interval8 != D3DPRESENT_INTERVAL_DEFAULT ||
                     rate != D3DPRESENT_RATE_DEFAULT)) {
        // D3D8 requires both full-screen fields to be 0 in windowed mode.
        hr = D3DERR_INVALIDCALL;
        return 0;
    }
    // Measured (d3d8_contract_probe, create.*): D3D8 has swap effects
    // DISCARD..COPY_VSYNC only, no D3DFMT_UNKNOWN back buffer even
    // windowed, and at most 3 back buffers; a larger count is clamped to 3
    // in the caller's structure and refused.
    if (swap < d3d8c::D3DSWAPEFFECT_DISCARD || swap > d3d8c::D3DSWAPEFFECT_COPY_VSYNC ||
        rd(at(pp8, L::BackBufferFormat.d3d8)) == fmt8::D3DFMT_UNKNOWN) {
        hr = D3DERR_INVALIDCALL;
        return 0;
    }
    constexpr u32 kMaxBackBuffers = 3;
    if (rd(at(pp8, L::BackBufferCount.d3d8)) > kMaxBackBuffers) {
        wr(at(pp8, L::BackBufferCount.d3d8), kMaxBackBuffers);
        hr = D3DERR_INVALIDCALL;
        return 0;
    }
    wr(at(pp9, L::SwapEffect.d3d9),
       swap == D3DSWAPEFFECT_COPY_VSYNC ? D3DSWAPEFFECT_COPY : swap);
    u32 interval9 = interval8;
    if (windowed) {
        interval9 = swap == D3DSWAPEFFECT_COPY_VSYNC ? D3DPRESENT_INTERVAL_ONE
                                                     : D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    wr(at(pp9, L::PresentationInterval.d3d9), interval9);
    if (!windowed && rate == D3DPRESENT_RATE_UNLIMITED) {
        wr(at(pp9, L::FullScreen_RefreshRateInHz.d3d9), D3DPRESENT_RATE_DEFAULT);
    }
    return pp9;
}

void D3D8Manager::present_parameters_from9(GuestAddr pp9, GuestAddr pp8) {
    namespace L = layout_PRESENT_PARAMETERS;
    // Measured: a successful CreateDevice writes back a BackBufferCount of
    // 0 as 1, and leaves a 0 width/height as 0 (D3D9 fills those in).
    wr(at(pp8, L::BackBufferCount.d3d8), rd(at(pp9, L::BackBufferCount.d3d9)));
}

// -----------------------------------------------------------------------------
// D3DCAPS8
// -----------------------------------------------------------------------------
//
// D3DCAPS8 is the first 212 bytes of D3DCAPS9 (same member names, same
// offsets: d3d8_contract.inc), ending with MaxPixelShaderValue where D3D9
// has PixelShader1xMaxValue. A D3D8 device reports no capability bit that
// d3d8caps.h cannot name, and shader versions D3D8 can create (vs 1.1,
// ps 1.4). D3D9 replaced D3DPRASTERCAPS_ZBIAS with DEPTHBIAS; the frontend
// implements D3DRS_ZBIAS through D3DRS_DEPTHBIAS, so a device with depth
// bias reports ZBIAS.

void D3D8Manager::caps_from9(GuestAddr caps9, GuestAddr caps8) {
    namespace L = layout_CAPS8;
    static_assert(L::SIZE8 == 212);
    static_assert(L::MaxPixelShaderValue.d3d8 == L::PixelShader1xMaxValue.d3d9);
    copy_guest(caps8, caps9, L::SIZE8);
    struct Mask {
        FieldPair field;
        u32 mask;
    };
    const Mask masks[] = {
        {L::Caps, kCaps8Mask_Caps},
        {L::Caps2, kCaps8Mask_Caps2},
        {L::Caps3, kCaps8Mask_Caps3},
        {L::PresentationIntervals, kCaps8Mask_PresentationIntervals},
        {L::CursorCaps, kCaps8Mask_CursorCaps},
        {L::DevCaps, kCaps8Mask_DevCaps},
        {L::PrimitiveMiscCaps, kCaps8Mask_PrimitiveMiscCaps},
        {L::RasterCaps, kCaps8Mask_RasterCaps},
        {L::ZCmpCaps, kCaps8Mask_ZCmpCaps},
        {L::SrcBlendCaps, kCaps8Mask_SrcBlendCaps},
        {L::DestBlendCaps, kCaps8Mask_DestBlendCaps},
        {L::AlphaCmpCaps, kCaps8Mask_AlphaCmpCaps},
        {L::ShadeCaps, kCaps8Mask_ShadeCaps},
        {L::TextureCaps, kCaps8Mask_TextureCaps},
        {L::TextureFilterCaps, kCaps8Mask_TextureFilterCaps},
        {L::CubeTextureFilterCaps, kCaps8Mask_CubeTextureFilterCaps},
        {L::VolumeTextureFilterCaps, kCaps8Mask_VolumeTextureFilterCaps},
        {L::TextureAddressCaps, kCaps8Mask_TextureAddressCaps},
        {L::VolumeTextureAddressCaps, kCaps8Mask_VolumeTextureAddressCaps},
        {L::LineCaps, kCaps8Mask_LineCaps},
        {L::StencilCaps, kCaps8Mask_StencilCaps},
        {L::FVFCaps, kCaps8Mask_FVFCaps},
        {L::TextureOpCaps, kCaps8Mask_TextureOpCaps},
        {L::VertexProcessingCaps, kCaps8Mask_VertexProcessingCaps},
    };
    for (const Mask& entry : masks) {
        const GuestAddr address = at(caps8, entry.field.d3d8);
        wr(address, rd(address) & entry.mask);
    }
    const u32 raster9 = rd(at(caps9, L::RasterCaps.d3d9));
    if ((raster9 & d3d9h::D3DPRASTERCAPS_DEPTHBIAS) != 0) {
        const GuestAddr address = at(caps8, L::RasterCaps.d3d8);
        wr(address, rd(address) | D3DPRASTERCAPS_ZBIAS);
    }
    const GuestAddr vs = at(caps8, L::VertexShaderVersion.d3d8);
    if (rd(vs) > D3DVS_VERSION_1_1) wr(vs, D3DVS_VERSION_1_1);
    const GuestAddr ps = at(caps8, L::PixelShaderVersion.d3d8);
    if (rd(ps) > D3DPS_VERSION_1_4) wr(ps, D3DPS_VERSION_1_4);
}

// -----------------------------------------------------------------------------
// D3DSURFACE_DESC / D3DVOLUME_DESC: D3D8 carries the resource's byte Size.
// -----------------------------------------------------------------------------

u32 D3D8Manager::surface_desc_from9(GuestAddr desc9, GuestAddr desc8) {
    namespace L = layout_SURFACE_DESC;
    for (const FieldPair field : {L::Format, L::Type, L::Usage, L::Pool,
                                  L::MultiSampleType, L::Width, L::Height}) {
        wr(at(desc8, field.d3d8), rd(at(desc9, field.d3d9)));
    }
    // Measured: the size in bytes of a lockable surface's storage; 0 for a
    // render target or depth stencil (the back buffer, the automatic depth
    // buffer), whose memory the runtime does not expose.
    // Rows are DWORD-aligned (measured: a 1x1 R5G6B5 level is 4 bytes).
    const u32 format = rd(at(desc9, L::Format.d3d9));
    const u32 width = rd(at(desc9, L::Width.d3d9));
    const u32 height = rd(at(desc9, L::Height.d3d9));
    u32 pitch = 0;
    usize bytes = 0;
    if ((rd(at(desc9, L::Usage.d3d9)) &
         (d3d8c::D3DUSAGE_RENDERTARGET | d3d8c::D3DUSAGE_DEPTHSTENCIL)) != 0) {
        bytes = 0;
    } else if (const u32 bpp = d3d9_format_bytes_per_pixel(format); bpp != 0) {
        bytes = static_cast<usize>((width * bpp + 3u) & ~3u) * height;
    } else if (!d3d9_surface_storage(format, width, height, pitch, bytes)) {
        bytes = 0;
    }
    wr(at(desc8, L::Size.d3d8), static_cast<u32>(bytes));
    return D3D_OK;
}

u32 D3D8Manager::volume_desc_from9(GuestAddr desc9, GuestAddr desc8) {
    namespace L = layout_VOLUME_DESC;
    for (const FieldPair field : {L::Format, L::Type, L::Usage, L::Pool,
                                  L::Width, L::Height, L::Depth}) {
        wr(at(desc8, field.d3d8), rd(at(desc9, field.d3d9)));
    }
    const u32 format = rd(at(desc9, L::Format.d3d9));
    const u32 width = rd(at(desc9, L::Width.d3d9));
    const u32 height = rd(at(desc9, L::Height.d3d9));
    const u32 depth = rd(at(desc9, L::Depth.d3d9));
    u32 row = 0;
    u32 slice = 0;
    usize bytes = 0;
    if (const u32 bpp = d3d9_format_bytes_per_pixel(format); bpp != 0) {
        // DWORD-aligned rows, as for surfaces.
        bytes = static_cast<usize>((width * bpp + 3u) & ~3u) * height * depth;
    } else if (!d3d9_volume_storage(format, width, height, depth, row, slice, bytes)) {
        bytes = 0;
    }
    wr(at(desc8, L::Size.d3d8), static_cast<u32>(bytes));
    return D3D_OK;
}

// -----------------------------------------------------------------------------
// Adapter modes
// -----------------------------------------------------------------------------
//
// Learn (EnumAdapterModes Changes): D3D9 made the caller name the display
// format; D3D8 enumerates every display mode of the adapter. The display
// formats are the D3D9 list Learn gives, enumerated once per bit depth
// (alpha and non-alpha variants enumerate identically, and 565/555 are
// one 16-bit mode set whose Format names the one the adapter uses).

u32 D3D8Manager::get_adapter_mode_list(u32 d3d9, u32 adapter,
                                       std::vector<std::array<u32, 4>>& modes) {
    modes.clear();

    const GuestAddr mode = scratch(16);
    if (mode == 0) return d3d8::E_OUTOFMEMORY;
    if (adapter >= call9(D3D9InterfaceId::Direct3D9,
                         D3D8_SLOT9(Direct3D9, GetAdapterCount), {d3d9})) {
        return D3DERR_INVALIDCALL;
    }
    // D3D8 display formats: one 16-bit set (565 or 555, the adapter's) and
    // X8R8G8B8; D3D8 has no 10-bit display modes.
    for (const u32 format : {fmt8::D3DFMT_R5G6B5, fmt8::D3DFMT_X8R8G8B8}) {
        const u32 count = call9(D3D9InterfaceId::Direct3D9,
                                D3D8_SLOT9(Direct3D9, GetAdapterModeCount),
                                {d3d9, adapter, format});
        for (u32 i = 0; i < count; ++i) {
            const u32 hr = call9(D3D9InterfaceId::Direct3D9,
                                 D3D8_SLOT9(Direct3D9, EnumAdapterModes),
                                 {d3d9, adapter, format, i, static_cast<u32>(mode)});
            if (failed(hr)) break;
            modes.push_back({rd(mode), rd(mode + 4), rd(mode + 8), rd(mode + 12)});
        }
    }
    return D3D_OK;
}

}  // namespace vitreo::winapi
