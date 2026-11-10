// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d8_device.cpp
/// @brief IDirect3DDevice8.
///
/// Every method maps onto the IDirect3DDevice9 method Learn's "Converting to
/// Direct3D 9" names as its successor, with the argument changes listed
/// there: the NULL shared handle added to resource creation, the swap-chain
/// index added to back-buffer/gamma/raster calls, the stream offset added to
/// Get/SetStreamSource, BaseVertexIndex moved from SetIndices to
/// DrawIndexedPrimitive, CreateImageSurface renamed CreateOffscreenPlainSurface,
/// ResourceManagerDiscardBytes replaced by EvictManagedResources, the
/// sampler-type texture-stage states moved to SetSamplerState, and
/// D3DRS_SOFTWAREVERTEXPROCESSING replaced by SetSoftwareVertexProcessing.

#include "winapi/graphics/d3d8/d3d8_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

using namespace d3d8c;

namespace {

bool failed(u32 hr) { return (hr & 0x80000000u) != 0; }

constexpr D3D9InterfaceId kDev = D3D9InterfaceId::Device;

// D3DRENDERSTATETYPE values D3D9 removed or renamed (kRenderStates8 rows
// whose D3D9 value is -1).
constexpr u32 RS_LINEPATTERN = 10;
constexpr u32 RS_ZVISIBLE = 30;
constexpr u32 RS_EDGEANTIALIAS = 40;
constexpr u32 RS_ZBIAS = 47;
constexpr u32 RS_SOFTWAREVERTEXPROCESSING = 153;
constexpr u32 RS_PATCHSEGMENTS = 164;
constexpr u32 RS_POSITIONORDER = 172;
constexpr u32 RS_NORMALORDER = 173;

/// D3D9 D3DRS_ANTIALIASEDLINEENABLE (d3d9types.h): what D3D8's
/// EDGEANTIALIAS (antialias the edges of lines) became.
constexpr u32 RS9_ANTIALIASEDLINEENABLE = 176;

const D3D8StateRow* render_state_row(u32 state) {
    for (const D3D8StateRow& row : kRenderStates8) {
        if (row.value == state) return &row;
    }
    return nullptr;
}

const D3D8StageStateRow* stage_state_row(u32 type) {
    for (const D3D8StageStateRow& row : kTextureStageStates8) {
        if (row.value == type) return &row;
    }
    return nullptr;
}

/// D3D8 texture stages (D3DCAPS8.MaxTextureBlendStages is at most 8).
constexpr u32 kMaxStages = 8;

/// The DirectX 7 render states D3D8 retired: GetRenderState refuses them
/// with D3DERR_INVALIDCALL while SetRenderState accepts them and changes
/// nothing (measured, tools/oracle_probe/d3d8_contract_probe32
/// .winoracle-gpu.expected, rs[]). They are TEXTUREHANDLE..WRAPV (1..6),
/// MONOENABLE/ROP2/PLANEMASK (11..13), TEXTUREMAG/MIN (17, 18),
/// TEXTUREMAPBLEND (21), SUBPIXEL/SUBPIXELX/STIPPLEDALPHA (31..33),
/// STIPPLEENABLE (39), COLORKEYENABLE (41), BORDERCOLOR..MIPMAPLODBIAS
/// (43..46), ANISOTROPY (49), TRANSLUCENTSORTINDEPENDENT (51),
/// STIPPLEPATTERN00..31 (64..95) and COLORKEYBLENDENABLE (144).
bool retired_render_state(u32 state) {
    if (state >= 1 && state <= 6) return true;
    if (state >= 11 && state <= 13) return true;
    if (state >= 64 && state <= 95) return true;
    switch (state) {
    case 17: case 18: case 21: case 31: case 32: case 33: case 39: case 41:
    case 43: case 44: case 45: case 46: case 49: case 51: case 144:
        return true;
    default:
        return false;
    }
}

/// D3DRS_DEBUGMONITORTOKEN: a debug-runtime state the retail runtime keeps
/// only for Get (measured: an unset Get writes nothing).
constexpr u32 RS_DEBUGMONITORTOKEN = 165;
constexpr u32 RS_POINTSIZE_MIN = 155;
constexpr u32 RS_POINTSIZE_MAX = 166;

}  // namespace

// =============================================================================
// Render states
// =============================================================================

void D3D8Manager::apply_creation_defaults(Object& dev) {
    // Measured (rs[155], rs[166]): a new D3D8 device starts with
    // POINTSIZE_MIN 0.0 and POINTSIZE_MAX equal to D3DCAPS8.MaxPointSize,
    // where D3D9 starts with 1.0 and 64.0.
    const GuestAddr caps9 = scratch(layout_CAPS8::SIZE9);
    if (caps9 == 0 || failed(call9(kDev, D3D8_SLOT9(Device, GetDeviceCaps),
                                   {dev.d3d9, static_cast<u32>(caps9)}))) {
        return;
    }
    (void)call9(kDev, D3D8_SLOT9(Device, SetRenderState), {dev.d3d9, RS_POINTSIZE_MIN, 0u});
    (void)call9(kDev, D3D8_SLOT9(Device, SetRenderState),
                {dev.d3d9, RS_POINTSIZE_MAX,
                 rd(at(caps9, layout_CAPS8::MaxPointSize.d3d9))});
}

u32 D3D8Manager::apply_zbias(Object& dev) {
    // D3DRS_ZBIAS k draws a primitive k units of the depth buffer's
    // resolution nearer: measured on D24S8 (k * 2^-24) and D16 (k * 2^-16),
    // tools/oracle_probe/d3d8_contract_probe "zbias2". D3D9 replaced it with
    // D3DRS_DEPTHBIAS, a normalized offset added to depth.
    const u32 zbias = dev.device->render_states8[RS_ZBIAS];
    u32 depth_bits = 24;
    const GuestAddr out9 = scratch(4);
    if (!failed(call9(kDev, D3D8_SLOT9(Device, GetDepthStencilSurface),
                      {dev.d3d9, static_cast<u32>(out9)}))) {
        const u32 surface = rd(out9);
        const GuestAddr desc = scratch(layout_SURFACE_DESC::SIZE9);
        if (surface != 0 &&
            !failed(call9(D3D9InterfaceId::Surface, D3D8_SLOT9(Surface, GetDesc),
                          {surface, static_cast<u32>(desc)}))) {
            switch (rd(at(desc, layout_SURFACE_DESC::Format.d3d9))) {
            case fmt8::D3DFMT_D16_LOCKABLE:
            case fmt8::D3DFMT_D16: depth_bits = 16; break;
            case fmt8::D3DFMT_D15S1: depth_bits = 15; break;
            case fmt8::D3DFMT_D32: depth_bits = 32; break;
            default: depth_bits = 24; break;
            }
        }
        if (surface != 0) (void)call9(D3D9InterfaceId::Surface, 2, {surface});
    }
    const float bias = -static_cast<float>(zbias) * std::ldexp(1.0f, -static_cast<int>(depth_bits));
    u32 bits = 0;
    std::memcpy(&bits, &bias, sizeof bits);
    return call9(kDev, D3D8_SLOT9(Device, SetRenderState),
                 {dev.d3d9, d3d9h::D3DRS_DEPTHBIAS, bits});
}

bool D3D8Manager::npatch_draw(Object& dev) {
    // PATCHSEGMENTS > 1.0 tessellates draws from a stream-0 vertex buffer
    // created with D3DUSAGE_NPATCHES. There is no tessellation path.
    const u32 segments = dev.device->render_states8[RS_PATCHSEGMENTS];
    float value = 0;
    std::memcpy(&value, &segments, sizeof value);
    if (!(value > 1.0f)) return false;
    const GuestAddr out9 = scratch(4);
    const GuestAddr offset = scratch(4);
    const GuestAddr stride = scratch(4);
    if (failed(call9(kDev, D3D8_SLOT9(Device, GetStreamSource),
                     {dev.d3d9, 0u, static_cast<u32>(out9), static_cast<u32>(offset),
                      static_cast<u32>(stride)})) ||
        rd(out9) == 0) {
        return false;
    }
    const u32 buffer = rd(out9);
    const GuestAddr desc = scratch(layout_VERTEXBUFFER_DESC::SIZE9);
    const bool npatches =
        !failed(call9(D3D9InterfaceId::VertexBuffer, D3D8_SLOT9(VertexBuffer, GetDesc),
                      {buffer, static_cast<u32>(desc)})) &&
        (rd(at(desc, layout_VERTEXBUFFER_DESC::Usage.d3d9)) & D3DUSAGE_NPATCHES) != 0;
    (void)call9(D3D9InterfaceId::VertexBuffer, 2, {buffer});
    if (npatches) {
        d3d8_log_unimplemented_once("IDirect3DDevice8::Draw*Primitive",
                                    "N-patch tessellation (D3DRS_PATCHSEGMENTS > 1)");
    }
    return npatches;
}

u32 D3D8Manager::device_set_render_state(Object& dev, u32 state, u32 value) {
    DeviceState& ds = *dev.device;
    const D3D8StateRow* row = render_state_row(state);
    // Measured: SetRenderState never fails. A state d3d8types.h does not
    // define is kept for Get (below 256), a retired one changes nothing.
    if (row == nullptr || state == RS_DEBUGMONITORTOKEN) {
        if (state < 256 && !retired_render_state(state)) {
            ds.unknown_render_states[state] = value;
        }
        return D3D_OK;
    }
    u32 hr = D3D_OK;
    switch (state) {
    case RS_LINEPATTERN:
    case RS_ZVISIBLE:
        // A device without D3DPRASTERCAPS_PAT / ZVISIBLE support (no D3D9
        // device has them; caps_from9 reports neither) keeps the value for
        // Get and draws as if they were at their defaults.
        break;
    case RS_EDGEANTIALIAS:
        hr = call9(kDev, D3D8_SLOT9(Device, SetRenderState),
                   {dev.d3d9, RS9_ANTIALIASEDLINEENABLE, value});
        break;
    case RS_ZBIAS:
        ds.render_states8[state] = value;
        hr = apply_zbias(dev);
        break;
    case RS_SOFTWAREVERTEXPROCESSING:
        hr = call9(kDev, D3D8_SLOT9(Device, SetSoftwareVertexProcessing),
                   {dev.d3d9, value != 0 ? 1u : 0u});
        break;
    case RS_PATCHSEGMENTS:
        // D3D8 tessellates N-patches only for buffers created with
        // D3DUSAGE_NPATCHES; D3D9's SetNPatchMode would apply it to every
        // draw, so the value is kept here and checked at draw time.
        break;
    case RS_POSITIONORDER:
        // Renamed D3DRS_POSITIONDEGREE; D3DORDER_* and D3DDEGREE_* share
        // values (LINEAR 1, CUBIC 3, QUINTIC 5).
        return call9(kDev, D3D8_SLOT9(Device, SetRenderState),
                     {dev.d3d9, d3d9h::D3DRS_POSITIONDEGREE, value});
    case RS_NORMALORDER:
        return call9(kDev, D3D8_SLOT9(Device, SetRenderState),
                     {dev.d3d9, d3d9h::D3DRS_NORMALDEGREE, value});
    default:
        if (row->d3d9 < 0) return D3DERR_INVALIDCALL;
        return call9(kDev, D3D8_SLOT9(Device, SetRenderState),
                     {dev.d3d9, static_cast<u32>(row->d3d9), value});
    }
    if (failed(hr)) return hr;
    ds.render_states8[state] = value;
    if (ds.recording) ds.recorded.render_states8[state] = value;
    return D3D_OK;
}

u32 D3D8Manager::device_get_render_state(Object& dev, u32 state, GuestAddr out) {
    DeviceState& ds = *dev.device;
    const D3D8StateRow* row = render_state_row(state);
    if (out == 0 || state >= 256 || retired_render_state(state)) {
        return D3DERR_INVALIDCALL;
    }
    if (row == nullptr || state == RS_DEBUGMONITORTOKEN) {
        // Measured: succeeds; a state Set never gave a value reads as the
        // runtime's fill value 0xBAADCAFE.
        const auto found = ds.unknown_render_states.find(state);
        wr(out, found != ds.unknown_render_states.end() ? found->second : 0xBAADCAFEu);
        return D3D_OK;
    }
    if (state == RS_POSITIONORDER || state == RS_NORMALORDER) {
        return call9(kDev, D3D8_SLOT9(Device, GetRenderState),
                     {dev.d3d9,
                      state == RS_POSITIONORDER ? d3d9h::D3DRS_POSITIONDEGREE
                                                : d3d9h::D3DRS_NORMALDEGREE,
                      static_cast<u32>(out)});
    }
    if (row->d3d9 < 0) {
        wr(out, ds.render_states8[state]);
        return D3D_OK;
    }
    return call9(kDev, D3D8_SLOT9(Device, GetRenderState),
                 {dev.d3d9, static_cast<u32>(row->d3d9), static_cast<u32>(out)});
}

// =============================================================================
// Texture stage states
// =============================================================================
//
// Learn (Texture Sampler Changes): D3D9 moved ADDRESSU/V/W, BORDERCOLOR,
// MAG/MIN/MIPFILTER, MIPMAPLODBIAS, MAXMIPLEVEL and MAXANISOTROPY to
// SetSamplerState as D3DSAMP_*; the rest stayed texture-stage states with
// their values. kTextureStageStates8 pairs each D3D8 type with its target.

u32 D3D8Manager::device_texture_stage_state(Object& dev, bool set, u32 stage,
                                            u32 type, u32 value, GuestAddr out) {
    if (!set && out == 0) return D3DERR_INVALIDCALL;
    const D3D8StageStateRow* row = stage_state_row(type);
    if (stage >= kMaxStages) {
        // Measured: Set at stage 8 succeeds and changes nothing; Get there
        // returns whatever lies past the runtime's stage array, which no
        // implementation can reproduce, so it is refused.
        if (set) return D3D_OK;
        d3d8_log_unimplemented_once("IDirect3DDevice8::GetTextureStageState",
                                    "stage beyond 7 (Windows reads past its array)");
        return D3DERR_INVALIDCALL;
    }
    if (row == nullptr) {
        // Types d3d8types.h does not define (0, 12, 29, 30, ...): measured,
        // Set succeeds and Get returns what Set stored, else 0xBAADCAFE.
        DeviceState& ds = *dev.device;
        const u32 key = (stage << 8) | (type & 0xFFu);
        if (type > 0xFFu) return set ? D3D_OK : D3DERR_INVALIDCALL;
        if (set) {
            ds.unknown_stage_states[key] = value;
        } else {
            const auto found = ds.unknown_stage_states.find(key);
            wr(out, found != ds.unknown_stage_states.end() ? found->second : 0xBAADCAFEu);
        }
        return D3D_OK;
    }
    if (row->d3d9_sampler >= 0) {
        const u32 sampler_state = static_cast<u32>(row->d3d9_sampler);
        return set ? call9(kDev, D3D8_SLOT9(Device, SetSamplerState),
                           {dev.d3d9, stage, sampler_state, value})
                   : call9(kDev, D3D8_SLOT9(Device, GetSamplerState),
                           {dev.d3d9, stage, sampler_state, static_cast<u32>(out)});
    }
    const u32 stage_state = static_cast<u32>(row->d3d9_stage);
    return set ? call9(kDev, D3D8_SLOT9(Device, SetTextureStageState),
                       {dev.d3d9, stage, stage_state, value})
               : call9(kDev, D3D8_SLOT9(Device, GetTextureStageState),
                       {dev.d3d9, stage, stage_state, static_cast<u32>(out)});
}

// =============================================================================
// CopyRects
// =============================================================================
//
// IDirect3DDevice8::CopyRects copies rectangles between two surfaces of the
// same format without stretching; NULL rects copy the whole source, NULL
// points place each rectangle at its source position. D3D9 split it by pool
// into UpdateSurface (system memory to default), StretchRect (default to
// default) and GetRenderTargetData; any pair of lockable surfaces copies
// through their locks.

u32 D3D8Manager::device_copy_rects(Object& dev, u32 src8, GuestAddr rects,
                                   u32 count, u32 dst8, GuestAddr points) {
    Object* src = find(src8, D3D8InterfaceId::Surface);
    Object* dst = find(dst8, D3D8InterfaceId::Surface);
    if (src == nullptr || dst == nullptr || src8 == dst8) return D3DERR_INVALIDCALL;
    namespace L = layout_SURFACE_DESC;
    const GuestAddr sd = scratch(L::SIZE9);
    const GuestAddr dd = scratch(L::SIZE9);
    if (failed(call9(D3D9InterfaceId::Surface, D3D8_SLOT9(Surface, GetDesc),
                     {src->d3d9, static_cast<u32>(sd)})) ||
        failed(call9(D3D9InterfaceId::Surface, D3D8_SLOT9(Surface, GetDesc),
                     {dst->d3d9, static_cast<u32>(dd)}))) {
        return D3DERR_INVALIDCALL;
    }
    const u32 format = rd(at(sd, L::Format.d3d9));
    if (format != rd(at(dd, L::Format.d3d9)) ||
        rd(at(sd, L::MultiSampleType.d3d9)) != 0 ||
        rd(at(dd, L::MultiSampleType.d3d9)) != 0) {
        return D3DERR_INVALIDCALL;
    }
    const u32 sw = rd(at(sd, L::Width.d3d9));
    const u32 sh = rd(at(sd, L::Height.d3d9));
    const u32 dw = rd(at(dd, L::Width.d3d9));
    const u32 dh = rd(at(dd, L::Height.d3d9));
    const u32 src_pool = rd(at(sd, L::Pool.d3d9));
    const u32 dst_pool = rd(at(dd, L::Pool.d3d9));
    if (static int shown = 0; std::getenv("VITREO_D3D8_CALL_LOG") != nullptr && shown < 12) {
        ++shown;
        std::fprintf(stderr,
                     "[d3d8-copyrects] src=%ux%u fmt=%u pool=%u usage=0x%x -> dst=%ux%u fmt=%u pool=%u "
                     "usage=0x%x rects=%u first=(%d,%d,%d,%d) point=(%d,%d)\n",
                     sw, sh, format, src_pool, rd(at(sd, L::Usage.d3d9)), dw, dh,
                     rd(at(dd, L::Format.d3d9)), dst_pool, rd(at(dd, L::Usage.d3d9)), count,
                     rects ? static_cast<i32>(rd(rects)) : 0, rects ? static_cast<i32>(rd(rects + 4)) : 0,
                     rects ? static_cast<i32>(rd(rects + 8)) : static_cast<i32>(sw),
                     rects ? static_cast<i32>(rd(rects + 12)) : static_cast<i32>(sh),
                     points ? static_cast<i32>(rd(points)) : 0, points ? static_cast<i32>(rd(points + 4)) : 0);
    }

    struct Copy {
        i32 l, t, r, b, x, y;
    };
    std::vector<Copy> copies;
    if (rects == 0) {
        Copy copy{0, 0, static_cast<i32>(sw), static_cast<i32>(sh), 0, 0};
        if (points != 0) {
            copy.x = static_cast<i32>(rd(points));
            copy.y = static_cast<i32>(rd(points + 4));
        }
        copies.push_back(copy);
    } else {
        if (count == 0) return D3DERR_INVALIDCALL;
        for (u32 i = 0; i < count; ++i) {
            const GuestAddr r = rects + 16u * i;
            Copy copy{static_cast<i32>(rd(r)), static_cast<i32>(rd(r + 4)),
                      static_cast<i32>(rd(r + 8)), static_cast<i32>(rd(r + 12)), 0, 0};
            copy.x = copy.l;
            copy.y = copy.t;
            if (points != 0) {
                copy.x = static_cast<i32>(rd(points + 8u * i));
                copy.y = static_cast<i32>(rd(points + 8u * i + 4));
            }
            copies.push_back(copy);
        }
    }
    for (const Copy& c : copies) {
        const i32 w = c.r - c.l;
        const i32 h = c.b - c.t;
        if (c.l < 0 || c.t < 0 || w <= 0 || h <= 0 || c.r > static_cast<i32>(sw) ||
            c.b > static_cast<i32>(sh) || c.x < 0 || c.y < 0 ||
            c.x + w > static_cast<i32>(dw) || c.y + h > static_cast<i32>(dh)) {
            return D3DERR_INVALIDCALL;
        }
    }

    const GuestAddr rect = scratch(16);
    const GuestAddr rect2 = scratch(16);
    const GuestAddr point = scratch(8);
    for (const Copy& c : copies) {
        const i32 w = c.r - c.l;
        const i32 h = c.b - c.t;
        wr(rect, static_cast<u32>(c.l));
        wr(rect + 4, static_cast<u32>(c.t));
        wr(rect + 8, static_cast<u32>(c.r));
        wr(rect + 12, static_cast<u32>(c.b));
        u32 hr = D3D_OK;
        if (src_pool != d3d8c::D3DPOOL_DEFAULT && dst_pool == d3d8c::D3DPOOL_DEFAULT) {
            wr(point, static_cast<u32>(c.x));
            wr(point + 4, static_cast<u32>(c.y));
            hr = call9(kDev, D3D8_SLOT9(Device, UpdateSurface),
                       {dev.d3d9, src->d3d9, static_cast<u32>(rect), dst->d3d9,
                        static_cast<u32>(point)});
        } else if (src_pool == d3d8c::D3DPOOL_DEFAULT && dst_pool == d3d8c::D3DPOOL_DEFAULT) {
            wr(rect2, static_cast<u32>(c.x));
            wr(rect2 + 4, static_cast<u32>(c.y));
            wr(rect2 + 8, static_cast<u32>(c.x + w));
            wr(rect2 + 12, static_cast<u32>(c.y + h));
            hr = call9(kDev, D3D8_SLOT9(Device, StretchRect),
                       {dev.d3d9, src->d3d9, static_cast<u32>(rect), dst->d3d9,
                        static_cast<u32>(rect2), 0u /* D3DTEXF_NONE */});
        } else {
            // Through the two surfaces' locks.
            u32 pitch_unused = 0;
            usize bytes_unused = 0;
            const bool compressed =
                d3d9_format_bytes_per_pixel(format) == 0 &&
                d3d9_surface_storage(format, 4, 4, pitch_unused, bytes_unused);
            const u32 block = compressed ? 4u : 1u;
            u32 row_bytes = 0;
            u32 rows = 0;
            if (compressed) {
                u32 block_pitch = 0;
                usize block_bytes = 0;
                (void)d3d9_surface_storage(format, 4, 4, block_pitch, block_bytes);
                row_bytes = static_cast<u32>(w + 3) / 4u * block_pitch;
                rows = static_cast<u32>(h + 3) / 4u;
                if (c.l % 4 != 0 || c.t % 4 != 0 || c.x % 4 != 0 || c.y % 4 != 0) {
                    return D3DERR_INVALIDCALL;
                }
            } else {
                const u32 bpp = d3d9_format_bytes_per_pixel(format);
                if (bpp == 0) return D3DERR_INVALIDCALL;
                row_bytes = static_cast<u32>(w) * bpp;
                rows = static_cast<u32>(h);
            }
            (void)block;
            const GuestAddr lock_src = scratch(8);
            const GuestAddr lock_dst = scratch(8);
            hr = call9(D3D9InterfaceId::Surface, D3D8_SLOT9(Surface, LockRect),
                       {src->d3d9, static_cast<u32>(lock_src), static_cast<u32>(rect),
                        d3d8c::D3DLOCK_READONLY});
            // Measured: copying out of a surface that cannot be read back
            // (the back buffer into an image surface) is E_FAIL.
            if (failed(hr)) return d3d8::E_FAIL;
            wr(rect2, static_cast<u32>(c.x));
            wr(rect2 + 4, static_cast<u32>(c.y));
            wr(rect2 + 8, static_cast<u32>(c.x + w));
            wr(rect2 + 12, static_cast<u32>(c.y + h));
            hr = call9(D3D9InterfaceId::Surface, D3D8_SLOT9(Surface, LockRect),
                       {dst->d3d9, static_cast<u32>(lock_dst), static_cast<u32>(rect2),
                        0u});
            if (!failed(hr)) {
                const u32 src_pitch = rd(lock_src);
                const GuestAddr src_bits = rd(lock_src + 4);
                const u32 dst_pitch = rd(lock_dst);
                const GuestAddr dst_bits = rd(lock_dst + 4);
                for (u32 y = 0; y < rows; ++y) {
                    copy_guest(dst_bits + static_cast<GuestAddr>(y) * dst_pitch,
                               src_bits + static_cast<GuestAddr>(y) * src_pitch,
                               row_bytes);
                }
                (void)call9(D3D9InterfaceId::Surface, D3D8_SLOT9(Surface, UnlockRect),
                            {dst->d3d9});
            }
            (void)call9(D3D9InterfaceId::Surface, D3D8_SLOT9(Surface, UnlockRect),
                        {src->d3d9});
        }
        if (failed(hr)) return hr;
    }
    return D3D_OK;
}

// =============================================================================
// State blocks
// =============================================================================
//
// D3D8 names a state block by a DWORD token. Each token owns one D3D9 state
// block plus the D3D8 state D3D9 cannot record: SetIndices' BaseVertexIndex
// and the render states D3D9 removed.

u32 D3D8Manager::device_state_block(Marshaler& m, usize slot, Object& dev) {
    DeviceState& ds = *dev.device;
    using S = DeviceSlot8;
    switch (static_cast<S>(slot)) {
    case S::BeginStateBlock: {
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, BeginStateBlock), {dev.d3d9});
        if (!failed(hr)) {
            ds.recording = true;
            ds.recorded = StateBlock{};
        }
        return hr;
    }
    case S::EndStateBlock: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return D3DERR_INVALIDCALL;
        const GuestAddr out9 = scratch(4);
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, EndStateBlock),
                             {dev.d3d9, static_cast<u32>(out9)});
        if (failed(hr)) return hr;
        ds.recording = false;
        StateBlock block = std::move(ds.recorded);
        ds.recorded = StateBlock{};
        block.block9 = rd(out9);
        const u32 token = ++ds.next_state_block;
        ds.state_blocks[token] = std::move(block);
        wr(out, token);
        return D3D_OK;
    }
    case S::CreateStateBlock: {
        const u32 type = m.arg32(1);
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) return D3DERR_INVALIDCALL;
        const GuestAddr out9 = scratch(4);
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, CreateStateBlock),
                             {dev.d3d9, type, static_cast<u32>(out9)});
        if (failed(hr)) return hr;
        StateBlock block;
        block.block9 = rd(out9);
        // D3DSBT_ALL holds every state; the index buffer belongs to it alone,
        // and of the D3D8-only render states SOFTWAREVERTEXPROCESSING and
        // PATCHSEGMENTS are vertex-pipeline state, the others pixel state.
        if (type == D3DSBT_ALL) {
            block.has_base_vertex_index = true;
            block.base_vertex_index = ds.base_vertex_index;
        }
        for (const auto& [state, value] : ds.render_states8) {
            const bool vertex = state == RS_SOFTWAREVERTEXPROCESSING ||
                                state == RS_PATCHSEGMENTS;
            if (type == D3DSBT_ALL || (vertex && type == D3DSBT_VERTEXSTATE) ||
                (!vertex && type == D3DSBT_PIXELSTATE)) {
                block.render_states8[state] = value;
            }
        }
        const u32 token = ++ds.next_state_block;
        ds.state_blocks[token] = std::move(block);
        wr(out, token);
        return D3D_OK;
    }
    case S::ApplyStateBlock:
    case S::CaptureStateBlock:
    case S::DeleteStateBlock: {
        // Measured: ApplyStateBlock(0) succeeds and changes nothing.
        if (static_cast<S>(slot) == S::ApplyStateBlock && m.arg32(1) == 0 &&
            !ds.recording) {
            return D3D_OK;
        }
        const auto found = ds.state_blocks.find(m.arg32(1));
        if (found == ds.state_blocks.end() || ds.recording) return D3DERR_INVALIDCALL;
        StateBlock& block = found->second;
        if (static_cast<S>(slot) == S::DeleteStateBlock) {
            (void)call9(D3D9InterfaceId::StateBlock, 2, {block.block9});
            ds.state_blocks.erase(found);
            return D3D_OK;
        }
        if (static_cast<S>(slot) == S::CaptureStateBlock) {
            const u32 hr = call9(D3D9InterfaceId::StateBlock,
                                 D3D8_SLOT9(StateBlock, Capture), {block.block9});
            if (failed(hr)) return hr;
            if (block.has_base_vertex_index) {
                block.base_vertex_index = ds.base_vertex_index;
            }
            for (auto& [state, value] : block.render_states8) {
                value = ds.render_states8[state];
            }
            return D3D_OK;
        }
        const u32 hr = call9(D3D9InterfaceId::StateBlock,
                             D3D8_SLOT9(StateBlock, Apply), {block.block9});
        if (failed(hr)) return hr;
        if (block.has_base_vertex_index) ds.base_vertex_index = block.base_vertex_index;
        const std::map<u32, u32> states = block.render_states8;
        for (const auto& [state, value] : states) {
            (void)device_set_render_state(dev, state, value);
        }
        return D3D_OK;
    }
    default:
        return D3DERR_INVALIDCALL;
    }
}

// =============================================================================
// Dispatch
// =============================================================================

void D3D8Manager::dispatch_device(Marshaler& m, usize slot, u32 self, Object& obj) {
    if (!obj.device) obj.device = std::make_unique<DeviceState>();
    if (dispatch_common(m, slot, self, obj)) return;
    log_device_call(m, slot);
    if (skip_draw_for_diagnosis(m, slot)) return;
    device_method(m, slot, self, obj);
}

bool D3D8Manager::skip_draw_for_diagnosis(Marshaler& m, usize slot) {
    // A third number 1 in the file also skips CopyRects.
    // VITREO_D3D8_DRAW_SKIP_FILE=<path>: the file holds "first last" scene
    // draw indices (0-based, per BeginScene) to skip, reread every 0.5 s, so
    // a running frame can be bisected for the draw that paints a region.
    static const char* path = std::getenv("VITREO_D3D8_DRAW_SKIP_FILE");
    if (path == nullptr) return false;
    using S = DeviceSlot8;
    const auto s = static_cast<S>(slot);
    static i64 first = -1, last = -1, index = 0, skip_copy = 0;
    static std::chrono::steady_clock::time_point loaded{};
    if (s == S::BeginScene) {
        index = 0;
        const auto now = std::chrono::steady_clock::now();
        if (now - loaded >= std::chrono::milliseconds(500)) {
            loaded = now;
            first = last = -1;
            skip_copy = 0;
            if (std::FILE* f = std::fopen(path, "r")) {
                long long a = -1, b = -1, c = 0;
                const int got = std::fscanf(f, "%lld %lld %lld", &a, &b, &c);
                if (got >= 2) {
                    first = a;
                    last = b;
                }
                skip_copy = got == 3 ? c : 0;
                std::fclose(f);
            }
        }
        return false;
    }
    if (s == S::CopyRects && skip_copy != 0) {
        m.set_return(D3D_OK);
        return true;
    }
    const bool draw = s == S::DrawPrimitive || s == S::DrawIndexedPrimitive ||
                      s == S::DrawPrimitiveUP || s == S::DrawIndexedPrimitiveUP;
    if (!draw) return false;
    const i64 i = index++;
    if (first < 0 || i < first || i > last) return false;
    m.set_return(D3D_OK);
    return true;
}

void D3D8Manager::log_device_call(Marshaler& m, usize slot) {
    // VITREO_D3D8_CALL_LOG=N: one scene every N seconds of wall time, each
    // device call between BeginScene and EndScene with its arguments (a whole
    // frame's state and draws, for comparing what the guest asked for with
    // what is drawn).
    static const int every_s = [] {
        const char* v = std::getenv("VITREO_D3D8_CALL_LOG");
        return v != nullptr ? std::max(1, std::atoi(v)) : 0;
    }();
    if (every_s == 0) return;
    static u64 frame = 0;
    static bool logging = false;
    static std::chrono::steady_clock::time_point last{};
    using S = DeviceSlot8;
    const auto s = static_cast<S>(slot);
    if (s == S::Present) ++frame;
    if (s == S::BeginScene) {
        const auto now = std::chrono::steady_clock::now();
        logging = now - last >= std::chrono::seconds(every_s);
        if (logging) last = now;
    }
    if (!logging) return;
    if (s == S::EndScene) logging = false;
    const std::string name = d3d8_method_export_name(D3D8InterfaceId::Device, slot);
    std::fprintf(stderr, "[d3d8-call] f=%llu %s %08x %08x %08x %08x %08x\n",
                 static_cast<unsigned long long>(frame), name.c_str(), m.arg32(1), m.arg32(2),
                 m.arg32(3), m.arg32(4), m.arg32(5));
    if (s == S::SetTransform && m.arg_ptr(2) != 0) {
        float v[16]{};
        for (int i = 0; i < 16; ++i) {
            const u32 bits = rd(m.arg_ptr(2) + 4u * static_cast<u32>(i));
            std::memcpy(&v[i], &bits, 4);
        }
        std::fprintf(stderr, "[d3d8-call]   m=[%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]\n",
                     v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
                     v[12], v[13], v[14], v[15]);
    }
}

void D3D8Manager::device_method(Marshaler& m, usize slot, u32 self, Object& obj) {
    (void)self;
    DeviceState& ds = *obj.device;
    const u32 d9 = obj.d3d9;
    const auto a = [&m](usize i) { return m.arg32(i); };
    const auto ret = [&m](u32 hr) { m.set_return(hr); };
    // Create*: append the NULL pShared handle, wrap the object returned.
    const auto create = [&](usize slot9, std::initializer_list<u32> args, GuestAddr out) {
        if (out == 0) {
            ret(D3DERR_INVALIDCALL);
            return;
        }
        wr(out, 0);
        const GuestAddr out9 = scratch(4);
        std::vector<u32> list{d9};
        list.insert(list.end(), args.begin(), args.end());
        list.push_back(static_cast<u32>(out9));
        list.push_back(0u);
        const u32 hr = call9v(kDev, slot9, list.data(), list.size());
        return_wrapped(out, out9, hr);
        ret(hr);
    };
    // Methods returning one object through their last argument.
    const auto get_object = [&](usize slot9, std::initializer_list<u32> args, GuestAddr out) {
        if (out == 0) {
            ret(D3DERR_INVALIDCALL);
            return;
        }
        const GuestAddr out9 = scratch(4);
        std::vector<u32> list{d9};
        list.insert(list.end(), args.begin(), args.end());
        list.push_back(static_cast<u32>(out9));
        const u32 hr = call9v(kDev, slot9, list.data(), list.size());
        return_wrapped(out, out9, hr);
        ret(hr);
    };

    using S = DeviceSlot8;
    switch (static_cast<S>(slot)) {
    case S::TestCooperativeLevel:
        return ret(call9(kDev, D3D8_SLOT9(Device, TestCooperativeLevel), {d9}));
    case S::GetAvailableTextureMem:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetAvailableTextureMem), {d9}));
    case S::ResourceManagerDiscardBytes:
        // Learn: replaced by EvictManagedResources, which evicts every
        // managed resource; a Bytes request is satisfied by evicting all.
        (void)a(1);
        return ret(call9(kDev, D3D8_SLOT9(Device, EvictManagedResources), {d9}));
    case S::GetDirect3D: {
        const GuestAddr out = m.arg_ptr(1);
        Object* d3d = find(ds.direct3d8, D3D8InterfaceId::Direct3D8);
        if (out == 0 || d3d == nullptr) return ret(D3DERR_INVALIDCALL);
        (void)call9(D3D9InterfaceId::Direct3D9, 1, {d3d->d3d9});
        wr(out, ds.direct3d8);
        return ret(D3D_OK);
    }
    case S::GetDeviceCaps: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return ret(D3DERR_INVALIDCALL);
        const GuestAddr caps9 = scratch(layout_CAPS8::SIZE9);
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, GetDeviceCaps),
                             {d9, static_cast<u32>(caps9)});
        if (!failed(hr)) {
            caps_from9(caps9, out);
            wr(at(out, layout_CAPS8::DeviceType.d3d8), ds.device_type);
        }
        return ret(hr);
    }
    case S::GetDisplayMode:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetDisplayMode), {d9, 0u, a(1)}));
    case S::GetCreationParameters:
        static_assert(layout_DEVICE_CREATION_PARAMETERS::SIZE8 ==
                      layout_DEVICE_CREATION_PARAMETERS::SIZE9);
    {
        // D3DDEVICE_CREATION_PARAMETERS { UINT AdapterOrdinal; D3DDEVTYPE
        // DeviceType; HWND hFocusWindow; DWORD BehaviorFlags; }: the type
        // the device was created as.
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, GetCreationParameters), {d9, a(1)});
        if (!failed(hr) && m.arg_ptr(1) != 0) {
            wr(at(m.arg_ptr(1), layout_DEVICE_CREATION_PARAMETERS::DeviceType.d3d8),
               ds.device_type);
        }
        return ret(hr);
    }
    case S::SetCursorProperties: {
        const u32 surface = a(3) == 0 ? 0u : unwrap(a(3));
        if (a(3) != 0 && surface == 0) return ret(D3DERR_INVALIDCALL);
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, SetCursorProperties),
                             {d9, a(1), a(2), surface});
        if (!failed(hr)) ds.cursor_set = true;
        return ret(hr);
    }
    case S::SetCursorPosition:
        (void)call9(kDev, D3D8_SLOT9(Device, SetCursorPosition), {d9, a(1), a(2), a(3)});
        return;
    case S::ShowCursor:
        // Measured (d3d8_device_cursor_probe): with no device cursor the call
        // leaves the Win32 cursor alone and answers FALSE; once one is set,
        // TRUE installs it as the Win32 cursor and FALSE sets none, and the
        // result is the previous visibility.
        if (!ds.cursor_set) return ret(0u);
        return ret(call9(kDev, D3D8_SLOT9(Device, ShowCursor), {d9, a(1)}));
    case S::CreateAdditionalSwapChain: {
        const GuestAddr pp8 = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(2);
        if (pp8 == 0 || out == 0) return ret(D3DERR_INVALIDCALL);
        wr(out, 0);
        u32 hr = D3D_OK;
        const GuestAddr pp9 = present_parameters_to9(pp8, hr);
        if (pp9 == 0) return ret(hr);
        const GuestAddr out9 = scratch(4);
        hr = call9(kDev, D3D8_SLOT9(Device, CreateAdditionalSwapChain),
                   {d9, static_cast<u32>(pp9), static_cast<u32>(out9)});
        if (!failed(hr)) present_parameters_from9(pp9, pp8);
        return_wrapped(out, out9, hr);
        return ret(hr);
    }
    case S::Reset: {
        const GuestAddr pp8 = m.arg_ptr(1);
        if (pp8 == 0) return ret(D3DERR_INVALIDCALL);
        u32 hr = D3D_OK;
        const GuestAddr pp9 = present_parameters_to9(pp8, hr);
        if (pp9 == 0) return ret(hr);
        hr = call9(kDev, D3D8_SLOT9(Device, Reset), {d9, static_cast<u32>(pp9)});
        if (!failed(hr)) {
            present_parameters_from9(pp9, pp8);
            // Reset returns every state to its creation value.
            apply_creation_defaults(obj);
            ds.unknown_render_states.clear();
            ds.unknown_stage_states.clear();
            ds.base_vertex_index = 0;
            for (auto& [state, value] : ds.render_states8) {
                value = state == RS_PATCHSEGMENTS ? 0x3F800000u
                        : state == RS_SOFTWAREVERTEXPROCESSING
                            ? ((ds.behavior_flags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0
                                   ? 1u
                                   : 0u)
                            : 0u;
            }
        }
        return ret(hr);
    }
    case S::Present:
        return ret(call9(kDev, D3D8_SLOT9(Device, Present), {d9, a(1), a(2), a(3), a(4)}));
    case S::GetBackBuffer:
        return get_object(D3D8_SLOT9(Device, GetBackBuffer), {0u, a(1), a(2)},
                          m.arg_ptr(3));
    case S::GetRasterStatus:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetRasterStatus), {d9, 0u, a(1)}));
    case S::SetGammaRamp:
        (void)call9(kDev, D3D8_SLOT9(Device, SetGammaRamp), {d9, 0u, a(1), a(2)});
        return;
    case S::GetGammaRamp:
        (void)call9(kDev, D3D8_SLOT9(Device, GetGammaRamp), {d9, 0u, a(1)});
        return;
    case S::CreateTexture:
        return create(D3D8_SLOT9(Device, CreateTexture),
                      {a(1), a(2), a(3), a(4), a(5), a(6)}, m.arg_ptr(7));
    case S::CreateVolumeTexture:
        return create(D3D8_SLOT9(Device, CreateVolumeTexture),
                      {a(1), a(2), a(3), a(4), a(5), a(6), a(7)}, m.arg_ptr(8));
    case S::CreateCubeTexture:
        return create(D3D8_SLOT9(Device, CreateCubeTexture),
                      {a(1), a(2), a(3), a(4), a(5)}, m.arg_ptr(6));
    case S::CreateVertexBuffer:
        return create(D3D8_SLOT9(Device, CreateVertexBuffer),
                      {a(1), a(2), a(3), a(4)}, m.arg_ptr(5));
    case S::CreateIndexBuffer:
        return create(D3D8_SLOT9(Device, CreateIndexBuffer),
                      {a(1), a(2), a(3), a(4)}, m.arg_ptr(5));
    case S::CreateRenderTarget:
        // (Width, Height, Format, MultiSample, Lockable): D3D9 adds the
        // multisample quality (0) before Lockable.
        return create(D3D8_SLOT9(Device, CreateRenderTarget),
                      {a(1), a(2), a(3), a(4), 0u, a(5)}, m.arg_ptr(6));
    case S::CreateDepthStencilSurface:
        // D3D9 adds MultisampleQuality (0) and Discard (FALSE: D3D8 depth
        // buffers keep their contents).
        return create(D3D8_SLOT9(Device, CreateDepthStencilSurface),
                      {a(1), a(2), a(3), a(4), 0u, 0u}, m.arg_ptr(5));
    case S::CreateImageSurface:
        // Learn: renamed CreateOffscreenPlainSurface with a pool; an image
        // surface is a lockable system-memory surface any CopyRects and
        // UpdateTexture source can use.
        return create(D3D8_SLOT9(Device, CreateOffscreenPlainSurface),
                      {a(1), a(2), a(3), d3d8c::D3DPOOL_SYSTEMMEM}, m.arg_ptr(4));
    case S::CopyRects:
        return ret(device_copy_rects(obj, a(1), m.arg_ptr(2), a(3), a(4), m.arg_ptr(5)));
    case S::UpdateTexture: {
        const u32 src = unwrap(a(1));
        const u32 dst = unwrap(a(2));
        if (src == 0 || dst == 0) return ret(D3DERR_INVALIDCALL);
        return ret(call9(kDev, D3D8_SLOT9(Device, UpdateTexture), {d9, src, dst}));
    }
    case S::GetFrontBuffer: {
        const u32 dst = unwrap(a(1));
        if (dst == 0) return ret(D3DERR_INVALIDCALL);
        return ret(call9(kDev, D3D8_SLOT9(Device, GetFrontBufferData), {d9, 0u, dst}));
    }
    case S::SetRenderTarget: {
        // NULL pRenderTarget keeps the current target; NULL pNewZStencil
        // binds no depth buffer.
        const u32 rt8 = a(1);
        const u32 zs8 = a(2);
        const u32 rt9 = rt8 != 0 ? unwrap(rt8) : 0u;
        const u32 zs9 = zs8 != 0 ? unwrap(zs8) : 0u;
        if ((rt8 != 0 && rt9 == 0) || (zs8 != 0 && zs9 == 0)) {
            return ret(D3DERR_INVALIDCALL);
        }
        if (rt9 != 0) {
            const u32 hr = call9(kDev, D3D8_SLOT9(Device, SetRenderTarget), {d9, 0u, rt9});
            if (failed(hr)) return ret(hr);
        }
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, SetDepthStencilSurface), {d9, zs9});
        // The depth bias ZBIAS stands for depends on the depth format.
        if (!failed(hr) && ds.render_states8[RS_ZBIAS] != 0) (void)apply_zbias(obj);
        return ret(hr);
    }
    case S::GetRenderTarget:
        return get_object(D3D8_SLOT9(Device, GetRenderTarget), {0u}, m.arg_ptr(1));
    case S::GetDepthStencilSurface:
        return get_object(D3D8_SLOT9(Device, GetDepthStencilSurface), {}, m.arg_ptr(1));
    case S::BeginScene:
        return ret(call9(kDev, D3D8_SLOT9(Device, BeginScene), {d9}));
    case S::EndScene:
        return ret(call9(kDev, D3D8_SLOT9(Device, EndScene), {d9}));
    case S::Clear:
        return ret(call9(kDev, D3D8_SLOT9(Device, Clear),
                         {d9, a(1), a(2), a(3), a(4), a(5), a(6)}));
    case S::SetTransform:
        return ret(call9(kDev, D3D8_SLOT9(Device, SetTransform), {d9, a(1), a(2)}));
    case S::GetTransform:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetTransform), {d9, a(1), a(2)}));
    case S::MultiplyTransform:
        return ret(call9(kDev, D3D8_SLOT9(Device, MultiplyTransform), {d9, a(1), a(2)}));
    case S::SetViewport:
        static_assert(layout_VIEWPORT8::SIZE8 == layout_VIEWPORT8::SIZE9);
        return ret(call9(kDev, D3D8_SLOT9(Device, SetViewport), {d9, a(1)}));
    case S::GetViewport:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetViewport), {d9, a(1)}));
    case S::SetMaterial:
        static_assert(layout_MATERIAL8::SIZE8 == layout_MATERIAL8::SIZE9);
        return ret(call9(kDev, D3D8_SLOT9(Device, SetMaterial), {d9, a(1)}));
    case S::GetMaterial:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetMaterial), {d9, a(1)}));
    case S::SetLight:
        static_assert(layout_LIGHT8::SIZE8 == layout_LIGHT8::SIZE9);
        return ret(call9(kDev, D3D8_SLOT9(Device, SetLight), {d9, a(1), a(2)}));
    case S::GetLight:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetLight), {d9, a(1), a(2)}));
    case S::LightEnable:
        return ret(call9(kDev, D3D8_SLOT9(Device, LightEnable), {d9, a(1), a(2)}));
    case S::GetLightEnable:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetLightEnable), {d9, a(1), a(2)}));
    case S::SetClipPlane:
        return ret(call9(kDev, D3D8_SLOT9(Device, SetClipPlane), {d9, a(1), a(2)}));
    case S::GetClipPlane:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetClipPlane), {d9, a(1), a(2)}));
    case S::SetRenderState:
        return ret(device_set_render_state(obj, a(1), a(2)));
    case S::GetRenderState:
        return ret(device_get_render_state(obj, a(1), m.arg_ptr(2)));
    case S::BeginStateBlock:
    case S::EndStateBlock:
    case S::ApplyStateBlock:
    case S::CaptureStateBlock:
    case S::DeleteStateBlock:
    case S::CreateStateBlock:
        return ret(device_state_block(m, slot, obj));
    case S::SetClipStatus:
        static_assert(layout_CLIPSTATUS8::SIZE8 == layout_CLIPSTATUS8::SIZE9);
        return ret(call9(kDev, D3D8_SLOT9(Device, SetClipStatus), {d9, a(1)}));
    case S::GetClipStatus:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetClipStatus), {d9, a(1)}));
    case S::GetTexture:
        if (a(1) >= kMaxStages) return ret(D3DERR_INVALIDCALL);
        return get_object(D3D8_SLOT9(Device, GetTexture), {a(1)}, m.arg_ptr(2));
    case S::SetTexture: {
        const u32 texture = a(2) != 0 ? unwrap(a(2)) : 0u;
        if (a(1) >= kMaxStages || (a(2) != 0 && texture == 0)) {
            return ret(D3DERR_INVALIDCALL);
        }
        return ret(call9(kDev, D3D8_SLOT9(Device, SetTexture), {d9, a(1), texture}));
    }
    case S::GetTextureStageState:
        return ret(device_texture_stage_state(obj, false, a(1), a(2), 0, m.arg_ptr(3)));
    case S::SetTextureStageState:
        return ret(device_texture_stage_state(obj, true, a(1), a(2), a(3), 0));
    case S::ValidateDevice:
        return ret(call9(kDev, D3D8_SLOT9(Device, ValidateDevice), {d9, a(1)}));
    case S::GetInfo: {
        // D3D9 replaced device info with queries (Learn). The resource
        // manager and vertex statistics come from the debug runtime; the
        // retail runtime reports the information as unavailable.
        // Measured with 4- and 4096-byte buffers: ids 4..7 (among them
        // D3DDEVINFOID_RESOURCEMANAGER 5 and VERTEXSTATS 6) are S_FALSE,
        // id 1 is E_FAIL.
        const u32 id = a(1);
        if (m.arg_ptr(2) == 0 || a(3) == 0) return ret(D3DERR_INVALIDCALL);
        static_assert(D3DDEVINFOID_RESOURCEMANAGER == 5 && D3DDEVINFOID_VERTEXSTATS == 6);
        if (id >= 4 && id <= 7) return ret(d3d8::S_FALSE);
        if (id != 1) {
            d3d8_log_unimplemented_once("IDirect3DDevice8::GetInfo",
                                        "device info id not measured");
        }
        return ret(d3d8::E_FAIL);
    }
    case S::SetPaletteEntries:
        return ret(call9(kDev, D3D8_SLOT9(Device, SetPaletteEntries), {d9, a(1), a(2)}));
    case S::GetPaletteEntries:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetPaletteEntries), {d9, a(1), a(2)}));
    case S::SetCurrentTexturePalette:
        return ret(call9(kDev, D3D8_SLOT9(Device, SetCurrentTexturePalette), {d9, a(1)}));
    case S::GetCurrentTexturePalette:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetCurrentTexturePalette), {d9, a(1)}));
    case S::DrawPrimitive:
        if (npatch_draw(obj)) return ret(d3d8::E_NOTIMPL);
        return ret(call9(kDev, D3D8_SLOT9(Device, DrawPrimitive), {d9, a(1), a(2), a(3)}));
    case S::DrawIndexedPrimitive:
        if (npatch_draw(obj)) return ret(d3d8::E_NOTIMPL);
        // Learn: D3D9 moved BaseVertexIndex from SetIndices to here.
        return ret(call9(kDev, D3D8_SLOT9(Device, DrawIndexedPrimitive),
                         {d9, a(1), ds.base_vertex_index, a(2), a(3), a(4), a(5)}));
    case S::DrawPrimitiveUP:
        return ret(call9(kDev, D3D8_SLOT9(Device, DrawPrimitiveUP),
                         {d9, a(1), a(2), a(3), a(4)}));
    case S::DrawIndexedPrimitiveUP:
        return ret(call9(kDev, D3D8_SLOT9(Device, DrawIndexedPrimitiveUP),
                         {d9, a(1), a(2), a(3), a(4), a(5), a(6), a(7), a(8)}));
    case S::ProcessVertices: {
        const u32 dest = unwrap(a(4));
        if (dest == 0) return ret(D3DERR_INVALIDCALL);
        return ret(call9(kDev, D3D8_SLOT9(Device, ProcessVertices),
                         {d9, a(1), a(2), a(3), dest, 0u, a(5)}));
    }
    case S::CreateVertexShader:
        return ret(create_vertex_shader(obj, m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3),
                                        a(4)));
    case S::SetVertexShader:
        return ret(set_vertex_shader(obj, a(1)));
    case S::GetVertexShader:
        return ret(get_vertex_shader(obj, m.arg_ptr(1)));
    case S::DeleteVertexShader:
        return ret(delete_vertex_shader(obj, a(1)));
    case S::SetVertexShaderConstant:
        return ret(call9(kDev, D3D8_SLOT9(Device, SetVertexShaderConstantF),
                         {d9, a(1), a(2), a(3)}));
    case S::GetVertexShaderConstant:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetVertexShaderConstantF),
                         {d9, a(1), a(2), a(3)}));
    case S::GetVertexShaderDeclaration:
        return ret(get_vertex_shader_blob(obj, a(1), true, m.arg_ptr(2), m.arg_ptr(3)));
    case S::GetVertexShaderFunction:
        return ret(get_vertex_shader_blob(obj, a(1), false, m.arg_ptr(2), m.arg_ptr(3)));
    case S::SetStreamSource: {
        const u32 buffer = a(2) != 0 ? unwrap(a(2)) : 0u;
        if (a(2) != 0 && buffer == 0) return ret(D3DERR_INVALIDCALL);
        return ret(call9(kDev, D3D8_SLOT9(Device, SetStreamSource),
                         {d9, a(1), buffer, 0u, a(3)}));
    }
    case S::GetStreamSource: {
        const GuestAddr out = m.arg_ptr(2);
        const GuestAddr stride = m.arg_ptr(3);
        if (out == 0 || stride == 0) return ret(D3DERR_INVALIDCALL);
        const GuestAddr out9 = scratch(4);
        const GuestAddr offset = scratch(4);
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, GetStreamSource),
                             {d9, a(1), static_cast<u32>(out9),
                              static_cast<u32>(offset), static_cast<u32>(stride)});
        return_wrapped(out, out9, hr);
        return ret(hr);
    }
    case S::SetIndices: {
        const u32 buffer = a(1) != 0 ? unwrap(a(1)) : 0u;
        if (a(1) != 0 && buffer == 0) return ret(D3DERR_INVALIDCALL);
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, SetIndices), {d9, buffer});
        if (failed(hr)) return ret(hr);
        ds.base_vertex_index = a(2);
        if (ds.recording) {
            ds.recorded.has_base_vertex_index = true;
            ds.recorded.base_vertex_index = a(2);
        }
        return ret(hr);
    }
    case S::GetIndices: {
        const GuestAddr out = m.arg_ptr(1);
        const GuestAddr base = m.arg_ptr(2);
        if (out == 0 || base == 0) return ret(D3DERR_INVALIDCALL);
        const GuestAddr out9 = scratch(4);
        const u32 hr = call9(kDev, D3D8_SLOT9(Device, GetIndices),
                             {d9, static_cast<u32>(out9)});
        return_wrapped(out, out9, hr);
        if (!failed(hr)) wr(base, ds.base_vertex_index);
        return ret(hr);
    }
    case S::CreatePixelShader:
        return ret(create_pixel_shader(obj, m.arg_ptr(1), m.arg_ptr(2)));
    case S::SetPixelShader:
        return ret(set_pixel_shader(obj, a(1)));
    case S::GetPixelShader:
        return ret(get_pixel_shader(obj, m.arg_ptr(1)));
    case S::DeletePixelShader:
        return ret(delete_pixel_shader(obj, a(1)));
    case S::SetPixelShaderConstant:
        return ret(call9(kDev, D3D8_SLOT9(Device, SetPixelShaderConstantF),
                         {d9, a(1), a(2), a(3)}));
    case S::GetPixelShaderConstant:
        return ret(call9(kDev, D3D8_SLOT9(Device, GetPixelShaderConstantF),
                         {d9, a(1), a(2), a(3)}));
    case S::GetPixelShaderFunction:
        return ret(get_pixel_shader_function(obj, a(1), m.arg_ptr(2), m.arg_ptr(3)));
    case S::DrawRectPatch:
        static_assert(layout_RECTPATCH_INFO::SIZE8 == layout_RECTPATCH_INFO::SIZE9);
        return ret(call9(kDev, D3D8_SLOT9(Device, DrawRectPatch), {d9, a(1), a(2), a(3)}));
    case S::DrawTriPatch:
        static_assert(layout_TRIPATCH_INFO::SIZE8 == layout_TRIPATCH_INFO::SIZE9);
        return ret(call9(kDev, D3D8_SLOT9(Device, DrawTriPatch), {d9, a(1), a(2), a(3)}));
    case S::DeletePatch:
        return ret(call9(kDev, D3D8_SLOT9(Device, DeletePatch), {d9, a(1)}));
    default:
        return ret(D3DERR_INVALIDCALL);
    }
}

}  // namespace vitreo::winapi
