// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d7_interface.cpp
/// @brief IDirect3D7: device enumeration and creation, vertex buffers and Z-buffer
///        formats.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "winapi/graphics/ddraw/ddraw_internal.hpp"
#include "graphics/contracts/cpu_surface.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstring>
#include <utility>

namespace vitreo::winapi {

using namespace ddraw_internal;

// =============================================================================
// IDirect3D7
// =============================================================================

bool DirectDrawManager::write_device_desc(GuestAddr out, u32 device_kind) {
    if (!m_memory || out == 0) return false;

    // D3DDEVICEDESC7 has no dwSize member, so the layout below is the only
    // thing keeping writes inside the caller's buffer. Derived from d3d.h:
    //   0   dwDevCaps
    //   4   dpcLineCaps   (D3DPRIMCAPS, 14 DWORDs = 56 bytes)
    //   60  dpcTriCaps    (D3DPRIMCAPS)
    //   116 dwDeviceRenderBitDepth      120 dwDeviceZBufferBitDepth
    //   124 dwMinTextureWidth           128 dwMinTextureHeight
    //   132 dwMaxTextureWidth           136 dwMaxTextureHeight
    //   140 dwMaxTextureRepeat          144 dwMaxTextureAspectRatio
    //   148 dwMaxAnisotropy
    //   152 dvGuardBandLeft/Top/Right/Bottom   168 dvExtentsAdjust
    //   172 dwStencilCaps               176 dwFVFCaps
    //   180 dwTextureOpCaps
    //   184 wMaxTextureBlendStages (WORD)  186 wMaxSimultaneousTextures (WORD)
    //   188 dwMaxActiveLights           192 dvMaxVertexW
    //   196 deviceGUID (16 bytes)
    //   212 dwWMaxUserClipPlanes        216 dwMaxVertexBlendMatrices
    //   220 dwVertexProcessingCaps      224 dwReserved1..4  (ends at 240)
    constexpr u32 DESC_SIZE = 240;
    constexpr u32 OFF_LINE_CAPS = 4;
    constexpr u32 OFF_TRI_CAPS = 60;
    constexpr u32 OFF_RENDER_BIT_DEPTH = 116;
    constexpr u32 OFF_ZBUFFER_BIT_DEPTH = 120;
    constexpr u32 OFF_MIN_TEXTURE_WIDTH = 124;
    constexpr u32 OFF_MAX_TEXTURE_WIDTH = 132;
    constexpr u32 OFF_MAX_TEXTURE_REPEAT = 140;
    constexpr u32 OFF_MAX_ANISOTROPY = 148;
    constexpr u32 OFF_FVF_CAPS = 176;
    constexpr u32 OFF_TEXTURE_OP_CAPS = 180;
    constexpr u32 OFF_MAX_BLEND_STAGES = 184;
    constexpr u32 OFF_MAX_ACTIVE_LIGHTS = 188;
    constexpr u32 OFF_MAX_VERTEX_W = 192;
    constexpr u32 OFF_DEVICE_GUID = 196;
    constexpr u32 OFF_MAX_USER_CLIP_PLANES = 212;
    constexpr u32 OFF_MAX_VERTEX_BLEND = 216;
    constexpr u32 OFF_VERTEX_PROCESSING_CAPS = 220;

    for (u32 offset = 0; offset < DESC_SIZE; offset += 4) {
        (void)m_memory->write_u32(out + offset, 0);
    }

    // Capability bits describe the fixed-function model d3d7_state.cpp
    // actually translates. Anything the translation does not express — user
    // clip-plane texgen, anisotropy, stencil — is deliberately left zero
    // rather than advertised.
    constexpr u32 D3DDEVCAPS_FLOATTLVERTEX = 0x00000001;
    constexpr u32 D3DDEVCAPS_TLVERTEXSYSTEMMEMORY = 0x00000040;
    constexpr u32 D3DDEVCAPS_TEXTURESYSTEMMEMORY = 0x00000100;
    constexpr u32 D3DDEVCAPS_DRAWPRIMTLVERTEX = 0x00000400;
    constexpr u32 D3DDEVCAPS_DRAWPRIMITIVE2 = 0x00002000;
    constexpr u32 D3DDEVCAPS_DRAWPRIMITIVE2EX = 0x00008000;
    constexpr u32 D3DDEVCAPS_HWTRANSFORMANDLIGHT = 0x00010000;
    constexpr u32 D3DDEVCAPS_TLVERTEXVIDEOMEMORY = 0x00000080;
    constexpr u32 D3DDEVCAPS_TEXTUREVIDEOMEMORY = 0x00000200;
    constexpr u32 D3DDEVCAPS_CANRENDERAFTERFLIP = 0x00000800;
    constexpr u32 D3DDEVCAPS_HWRASTERIZATION = 0x00080000;
    u32 dev_caps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                   D3DDEVCAPS_TEXTURESYSTEMMEMORY |
                   D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_DRAWPRIMITIVE2 |
                   D3DDEVCAPS_DRAWPRIMITIVE2EX;
    // HAL and TnLHAL are backed by the Metal pipeline, so they rasterize in
    // hardware and hold textures and vertex data in GPU memory. Reporting only
    // the *SYSTEMMEMORY caps describes a software rasterizer, and a caller
    // looking for a hardware device finds none and reports that it could not
    // initialize the graphics card. The RGB device is genuinely software
    // emulation and keeps the software-only shape.
    if (device_kind != static_cast<u32>(DeviceKind::Rgb)) {
        dev_caps |= D3DDEVCAPS_HWRASTERIZATION |
                    D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                    D3DDEVCAPS_TLVERTEXVIDEOMEMORY |
                    D3DDEVCAPS_CANRENDERAFTERFLIP;
    }
    // The TnLHAL device is the one that promises the driver transforms and
    // lights vertices itself; the HAL and RGB devices leave that to the
    // runtime, which is the same distinction the caller uses to decide
    // whether to pre-transform.
    if (device_kind == static_cast<u32>(DeviceKind::TnLHal)) {
        dev_caps |= D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    }
    (void)m_memory->write_u32(out, dev_caps);

    // D3DPRIMCAPS, shared by the line and triangle entries.
    constexpr u32 PRIMCAPS_SIZE = 56;
    constexpr u32 D3DPMISCCAPS_MASKZ = 0x00000002;
    constexpr u32 D3DPMISCCAPS_CULLNONE = 0x00000010;
    constexpr u32 D3DPMISCCAPS_CULLCW = 0x00000020;
    constexpr u32 D3DPMISCCAPS_CULLCCW = 0x00000040;
    constexpr u32 D3DPRASTERCAPS_ZTEST = 0x00000010;
    constexpr u32 D3DPRASTERCAPS_FOGVERTEX = 0x00000080;
    constexpr u32 D3DPRASTERCAPS_FOGTABLE = 0x00000100;
    constexpr u32 D3DPRASTERCAPS_FOGRANGE = 0x00010000;
    // Every comparison function (NEVER..ALWAYS) and every alpha-test function.
    constexpr u32 ALL_COMPARE_CAPS = 0x000000FF;
    // ZERO..SRCALPHASAT — the blend factors the backend contract exposes.
    constexpr u32 BLEND_CAPS = 0x000007FF;
    constexpr u32 D3DPSHADECAPS_COLORFLATRGB = 0x00000002;
    constexpr u32 D3DPSHADECAPS_COLORGOURAUDRGB = 0x00000008;
    constexpr u32 D3DPSHADECAPS_SPECULARFLATRGB = 0x00000080;
    constexpr u32 D3DPSHADECAPS_SPECULARGOURAUDRGB = 0x00000200;
    constexpr u32 D3DPSHADECAPS_ALPHAFLATBLEND = 0x00001000;
    constexpr u32 D3DPSHADECAPS_ALPHAGOURAUDBLEND = 0x00004000;
    constexpr u32 D3DPSHADECAPS_FOGFLAT = 0x00040000;
    constexpr u32 D3DPSHADECAPS_FOGGOURAUD = 0x00080000;
    constexpr u32 D3DPTEXTURECAPS_PERSPECTIVE = 0x00000001;
    constexpr u32 D3DPTEXTURECAPS_ALPHA = 0x00000004;
    constexpr u32 D3DPTEXTURECAPS_TRANSPARENCY = 0x00000008;
    // Not setting D3DPTEXTURECAPS_POW2 is what tells the caller that
    // non-power-of-two textures are allowed.
    constexpr u32 D3DPTFILTERCAPS_POINT_LINEAR_MIP = 0x0000003F;
    constexpr u32 D3DPTFILTERCAPS_MINF = 0x00000300;
    constexpr u32 D3DPTFILTERCAPS_MIPF = 0x00030000;
    constexpr u32 D3DPTFILTERCAPS_MAGF = 0x03000000;
    constexpr u32 D3DPTADDRESSCAPS_ALL = 0x0000001F;  // WRAP..INDEPENDENTUV

    const auto write_primcaps = [&](u32 base) {
        (void)m_memory->write_u32(out + base + 0, PRIMCAPS_SIZE);
        (void)m_memory->write_u32(out + base + 4,
                                  D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE |
                                  D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW);
        (void)m_memory->write_u32(out + base + 8,
                                  D3DPRASTERCAPS_ZTEST |
                                  D3DPRASTERCAPS_FOGVERTEX |
                                  D3DPRASTERCAPS_FOGTABLE |
                                  D3DPRASTERCAPS_FOGRANGE);
        (void)m_memory->write_u32(out + base + 12, ALL_COMPARE_CAPS);
        (void)m_memory->write_u32(out + base + 16, BLEND_CAPS);
        (void)m_memory->write_u32(out + base + 20, BLEND_CAPS);
        (void)m_memory->write_u32(out + base + 24, ALL_COMPARE_CAPS);
        (void)m_memory->write_u32(out + base + 28,
                                  D3DPSHADECAPS_COLORFLATRGB |
                                  D3DPSHADECAPS_COLORGOURAUDRGB |
                                  D3DPSHADECAPS_SPECULARFLATRGB |
                                  D3DPSHADECAPS_SPECULARGOURAUDRGB |
                                  D3DPSHADECAPS_ALPHAFLATBLEND |
                                  D3DPSHADECAPS_ALPHAGOURAUDBLEND |
                                  D3DPSHADECAPS_FOGFLAT |
                                  D3DPSHADECAPS_FOGGOURAUD);
        (void)m_memory->write_u32(out + base + 32,
                                  D3DPTEXTURECAPS_PERSPECTIVE |
                                  D3DPTEXTURECAPS_ALPHA |
                                  D3DPTEXTURECAPS_TRANSPARENCY);
        (void)m_memory->write_u32(out + base + 36,
                                  D3DPTFILTERCAPS_POINT_LINEAR_MIP |
                                  D3DPTFILTERCAPS_MINF |
                                  D3DPTFILTERCAPS_MIPF |
                                  D3DPTFILTERCAPS_MAGF);
        // dwTextureBlendCaps at +40 describes the DX3-era D3DTBLEND_* modes,
        // which SetTextureStageState replaced; left zero because the
        // translation implements the stage combiners, not those modes.
        (void)m_memory->write_u32(out + base + 44, D3DPTADDRESSCAPS_ALL);
    };
    write_primcaps(OFF_LINE_CAPS);
    write_primcaps(OFF_TRI_CAPS);

    // DDBD_16 | DDBD_32 for colour, plus DDBD_24 for depth.
    constexpr u32 DDBD_32 = 0x00000100;
    constexpr u32 DDBD_24 = 0x00000200;
    constexpr u32 DDBD_16 = 0x00000400;
    (void)m_memory->write_u32(out + OFF_RENDER_BIT_DEPTH, DDBD_16 | DDBD_32);
    (void)m_memory->write_u32(out + OFF_ZBUFFER_BIT_DEPTH,
                              DDBD_16 | DDBD_24 | DDBD_32);
    (void)m_memory->write_u32(out + OFF_MIN_TEXTURE_WIDTH, 1);
    (void)m_memory->write_u32(out + OFF_MIN_TEXTURE_WIDTH + 4, 1);
    (void)m_memory->write_u32(out + OFF_MAX_TEXTURE_WIDTH, 4096);
    (void)m_memory->write_u32(out + OFF_MAX_TEXTURE_WIDTH + 4, 4096);
    (void)m_memory->write_u32(out + OFF_MAX_TEXTURE_REPEAT, 4096);
    // dwMaxTextureAspectRatio 0 means unconstrained.
    (void)m_memory->write_u32(out + OFF_MAX_ANISOTROPY, 1);

    // dwFVFCaps' low 16 bits are the simultaneous texture-coordinate set count.
    (void)m_memory->write_u32(out + OFF_FVF_CAPS, d3d7::MAX_TEXTURE_STAGES);
    // Bits 0..23 are D3DTOP_DISABLE..D3DTOP_DOTPRODUCT3, the full D3D7 range;
    // MULTIPLYADD and LERP only exist from D3D8 on.
    (void)m_memory->write_u32(out + OFF_TEXTURE_OP_CAPS, 0x00FFFFFFu);
    // Two adjacent WORDs: wMaxTextureBlendStages, wMaxSimultaneousTextures.
    (void)m_memory->write_u32(
        out + OFF_MAX_BLEND_STAGES,
        d3d7::MAX_TEXTURE_STAGES | (d3d7::MAX_TEXTURE_STAGES << 16));
    (void)m_memory->write_u32(out + OFF_MAX_ACTIVE_LIGHTS,
                              d3d7::MAX_ACTIVE_LIGHTS);
    (void)m_memory->write_u32(out + OFF_MAX_VERTEX_W, 0x501502F9u);  // 1e10f

    const Guid& device_guid =
        device_kind == static_cast<u32>(DeviceKind::TnLHal)
            ? IID_IDIRECT3DTNLHALDEVICE
            : (device_kind == static_cast<u32>(DeviceKind::Rgb)
                   ? IID_IDIRECT3DRGBDEVICE
                   : IID_IDIRECT3DHALDEVICE);
    (void)m_memory->write_bytes(out + OFF_DEVICE_GUID,
                                reinterpret_cast<const Byte*>(&device_guid),
                                sizeof(Guid));

    (void)m_memory->write_u32(out + OFF_MAX_USER_CLIP_PLANES,
                              d3d7::MAX_CLIP_PLANES);
    // D3DRENDERSTATE_VERTEXBLEND tops out at three weights, which blends four
    // world matrices.
    (void)m_memory->write_u32(out + OFF_MAX_VERTEX_BLEND, 4);
    constexpr u32 D3DVTXPCAPS_MATERIALSOURCE7 = 0x00000002;
    constexpr u32 D3DVTXPCAPS_VERTEXFOG = 0x00000004;
    constexpr u32 D3DVTXPCAPS_DIRECTIONALLIGHTS = 0x00000008;
    constexpr u32 D3DVTXPCAPS_POSITIONALLIGHTS = 0x00000010;
    constexpr u32 D3DVTXPCAPS_LOCALVIEWER = 0x00000020;
    // D3DVTXPCAPS_TEXGEN stays off: the translation passes the raw
    // D3DTSS_TEXCOORDINDEX through but never generates coordinates.
    (void)m_memory->write_u32(out + OFF_VERTEX_PROCESSING_CAPS,
                              D3DVTXPCAPS_MATERIALSOURCE7 |
                              D3DVTXPCAPS_VERTEXFOG |
                              D3DVTXPCAPS_DIRECTIONALLIGHTS |
                              D3DVTXPCAPS_POSITIONALLIGHTS |
                              D3DVTXPCAPS_LOCALVIEWER);
    return true;
}

u32 DirectDrawManager::d3d7_enum_devices(GuestAddr object, GuestAddr callback,
                                         GuestAddr context) {
    if (!m_memory || !find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (callback == 0) return ddraw::E_INVALIDARG;
    if (!m_guest_call) return ddraw::E_NOTIMPL;
    if (!ensure_storage()) return 0x8007000Eu;

    // LPD3DENUMDEVICESCALLBACK7 is
    //   (LPSTR description, LPSTR name, LPD3DDEVICEDESC7, LPVOID context)
    // and returns D3DENUMRET_CANCEL (0) to stop the walk.
    constexpr u32 D3DENUMRET_CANCEL = 0;
    struct DeviceEntry {
        DeviceKind kind;
        const char* description;
        const char* name;
    };
    // The devices reported are the ones CreateDevice will actually build.
    // The reference rasterizer and the DX3-era ramp/MMX devices are not
    // listed because this implementation does not serve them.
    static constexpr DeviceEntry kDevices[] = {
        {DeviceKind::TnLHal, "Vitreo Hardware Transform and Lighting Device",
         "Vitreo T&L HAL"},
        {DeviceKind::Hal, "Vitreo Hardware Acceleration Device", "Vitreo HAL"},
        {DeviceKind::Rgb, "Vitreo RGB Emulation Device", "Vitreo RGB"},
    };

    // Scratch holds the two strings and the D3DDEVICEDESC7 the callback is
    // handed; the pointers must be guest-visible for the duration of the call.
    constexpr u32 SCRATCH_SIZE = 0x1000;
    constexpr u32 OFF_DESCRIPTION = 0;
    constexpr u32 OFF_NAME = 0x100;
    constexpr u32 OFF_DESC = 0x200;
    const GuestAddr scratch = m_memory->find_free_gap(0x62900000, SCRATCH_SIZE);
    if (scratch == 0 ||
        m_memory->map_region(scratch, SCRATCH_SIZE,
                             MemoryProtection::ReadWrite).is_err()) {
        return 0x8007000Eu;
    }

    u32 enumerated = 0;
    for (const auto& entry : kDevices) {
        const auto write_string = [&](u32 offset, const char* text) {
            const usize length = std::strlen(text) + 1;
            (void)m_memory->write_bytes(scratch + offset,
                                        reinterpret_cast<const Byte*>(text),
                                        length);
        };
        write_string(OFF_DESCRIPTION, entry.description);
        write_string(OFF_NAME, entry.name);
        if (!write_device_desc(scratch + OFF_DESC,
                               static_cast<u32>(entry.kind))) {
            (void)m_memory->unmap_region(scratch, SCRATCH_SIZE);
            return 0x8007000Eu;
        }
        const u64 args[4] = {static_cast<u64>(scratch + OFF_DESCRIPTION),
                             static_cast<u64>(scratch + OFF_NAME),
                             static_cast<u64>(scratch + OFF_DESC),
                             static_cast<u64>(context)};
        const u64 result = m_guest_call(callback, args, 4, false);
        ++enumerated;
        if (static_cast<u32>(result) == D3DENUMRET_CANCEL) {
            VITREO_TRACE("[d3d7] EnumDevices cancelled after %u\n", enumerated);
            (void)m_memory->unmap_region(scratch, SCRATCH_SIZE);
            return ddraw::DD_OK;
        }
    }
    VITREO_TRACE("[d3d7] EnumDevices enumerated %u devices\n", enumerated);
    (void)m_memory->unmap_region(scratch, SCRATCH_SIZE);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::d3d7_create_device(GuestAddr object, GuestAddr clsid,
                                          GuestAddr surface,
                                          GuestAddr out_device) {
    if (!m_memory || !find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (out_device == 0 || clsid == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(out_device, 0);

    Guid requested{};
    if (m_memory->read_bytes(clsid, reinterpret_cast<Byte*>(&requested),
                             sizeof(requested)).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    u32 kind = 0;
    if (guid_equal(requested, IID_IDIRECT3DTNLHALDEVICE)) {
        kind = static_cast<u32>(DeviceKind::TnLHal);
    } else if (guid_equal(requested, IID_IDIRECT3DHALDEVICE)) {
        kind = static_cast<u32>(DeviceKind::Hal);
    } else if (guid_equal(requested, IID_IDIRECT3DRGBDEVICE)) {
        kind = static_cast<u32>(DeviceKind::Rgb);
    } else {
        // Learn CreateDevice: an unsupported device GUID is
        // DDERR_INVALIDPARAMS. The ramp, MMX, reference and null devices fall
        // here because EnumDevices does not report them either.
        VITREO_TRACE("[d3d7] CreateDevice unsupported guid=%08x-%04x-%04x\n",
                     requested.data1, requested.data2, requested.data3);
        return ddraw::DDERR_INVALIDPARAMS;
    }

    // The render target must be a surface this DirectDraw object owns; the
    // device draws into its pixels and its size is the default viewport.
    SurfaceObject* target = find_surface(surface);
    if (target == nullptr) return ddraw::DDERR_INVALIDPARAMS;
    if (!ensure_storage()) return 0x8007000Eu;

    const GuestAddr device = m_next_object;
    m_next_object += 0x20;
    if (m_memory->write_u32(device,
                            static_cast<u32>(m_d3d7_device_vtable)).is_err() ||
        m_memory->write_u32(out_device, static_cast<u32>(device)).is_err()) {
        return 0x8007000Eu;
    }

    DeviceObject state;
    state.owner = object;
    state.kind = kind;
    state.state.render_target = surface;
    // A freshly created device's viewport covers the whole render target,
    // which is what a caller that never calls SetViewport relies on.
    state.state.viewport.width = target->surface ? target->surface->width() : 0;
    state.state.viewport.height = target->surface ? target->surface->height() : 0;
    state.state.viewport.max_z = 1.0f;
    m_devices.emplace(device, std::move(state));
    (void)add_ref(object);
    (void)surface_add_ref(surface);
    VITREO_TRACE("[d3d7] CreateDevice kind=%u surface=0x%llx -> device=0x%llx\n",
                 kind, static_cast<unsigned long long>(surface),
                 static_cast<unsigned long long>(device));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::d3d7_create_vertex_buffer(GuestAddr object,
                                                 GuestAddr desc,
                                                 GuestAddr out_buffer,
                                                 u32 flags) {
    if (!m_memory || !find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (out_buffer != 0) (void)m_memory->write_u32(out_buffer, 0);
    (void)desc; (void)flags;
    return ddraw::DDERR_INVALIDPARAMS;
}

u32 DirectDrawManager::d3d7_enum_z_buffer_formats(GuestAddr object,
                                                  GuestAddr clsid,
                                                  GuestAddr callback,
                                                  GuestAddr context) {
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    (void)clsid; (void)callback; (void)context;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::d3d7_evict_managed_textures(GuestAddr object) {
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    return ddraw::DD_OK;
}

} // namespace vitreo::winapi
