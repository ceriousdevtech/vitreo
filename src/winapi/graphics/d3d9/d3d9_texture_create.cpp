// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_texture_create.cpp
/// @brief D3D9 texture object creation: CreateTexture, CreateCubeTexture,
///        CreateVolumeTexture and the IDirect3DVolume9 level objects.
///
/// The IDirect3DDevice9 Create* slots reach these members from
/// d3d9_device_create.cpp.

#include "winapi/graphics/d3d9/d3d9_resources_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

namespace vitreo::winapi {

using namespace d3d9_resources_detail;

// =============================================================================
// IDirect3DTexture9 / IDirect3DCubeTexture9
// =============================================================================

void D3D9Manager::create_texture(Marshaler& m, u32 device) {
    const auto create_started = std::chrono::steady_clock::now();
    const u32 width = static_cast<u32>(m.arg(1));
    const u32 height = static_cast<u32>(m.arg(2));
    u32 levels = static_cast<u32>(m.arg(3));
    const u32 usage = static_cast<u32>(m.arg(4));
    const u32 format = static_cast<u32>(m.arg(5));
    const u32 pool = static_cast<u32>(m.arg(6));
    const GuestAddr out = m.arg_ptr(7);
    const GuestAddr shared_handle = m.arg_ptr(8);

    if (out != 0) (void)m.write_ptr(out, 0);
    if (width == 0 || height == 0 || out == 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (shared_handle != 0) {
        VITREO_TRACE("[d3d9] CreateTexture -> E_NOTIMPL "
                     "(no shared-resource handles)\n");
        m.set_return(E_NOTIMPL_D3D);
        return;
    }

    const graphics::PixelFormat backend_format = d3d9_backend_format(format);
    const bool depth_stencil = (usage & 0x2u /* D3DUSAGE_DEPTHSTENCIL */) != 0;
    const bool depth_format = backend_format == graphics::PixelFormat::D16Unorm ||
        backend_format == graphics::PixelFormat::D32Float ||
        backend_format == graphics::PixelFormat::D32Float_S8Uint;
    if (depth_stencil != depth_format ||
        (depth_stencil && (pool != D3DPOOL_DEFAULT ||
                         (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC)) != 0))) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    u32 storage_pitch = 0;
    usize storage_bytes = 0;
    if (backend_format == graphics::PixelFormat::Unknown ||
        (!depth_stencil && !d3d9_surface_storage(
            format, width, height, storage_pitch, storage_bytes))) {
        VITREO_TRACE("[d3d9] CreateTexture -> D3DERR_NOTAVAILABLE "
                     "(D3DFORMAT %u has no shared-core pixel format)\n",
                     format);
        m.set_return(D3DERR_NOTAVAILABLE);
        return;
    }
    (void)storage_pitch;
    (void)storage_bytes;

    // Zero means "generate the full mip chain".
    u32 max_levels = 1;
    for (u32 extent = std::max(width, height); extent > 1; extent >>= 1) {
        ++max_levels;
    }
    if (levels == 0) levels = max_levels;
    if (levels > max_levels) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    if (m_backend == nullptr || !m_backend->available()) {
        m.set_return(D3DERR_NOTAVAILABLE);
        return;
    }

    graphics::BackendTexture2DDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.mip_levels = levels;
    desc.array_size = 1;
    desc.pixel_format = backend_format;
    desc.shader_resource = true;
    desc.depth_stencil = depth_stencil;
    desc.render_target = (usage & D3DUSAGE_RENDERTARGET) != 0;
    // Learn: DEFAULT textures are not CPU-lockable unless D3DUSAGE_DYNAMIC.
    // MANAGED/SYSTEMMEM LockRect is a guest-memory copy; UnlockRect blits
    // into Private Metal storage. Shared + create-time zero-fill of every
    // mip is what ballooned host RSS to tens of gigabytes.
    desc.cpu_writable = (usage & D3DUSAGE_DYNAMIC) != 0;
    // Learn D3DFORMAT: L8 samples as (L,L,L,1); A8L8 as (L,L,L,A). Metal
    // R8/RG8 need an explicit channel swizzle to match.
    if (format == 50u || format == 81u) { // D3DFMT_L8 / D3DFMT_L16
        desc.channel_swizzle =
            graphics::BackendTexture2DDesc::ChannelSwizzle::Luminance;
    } else if (format == 51u) { // D3DFMT_A8L8
        desc.channel_swizzle =
            graphics::BackendTexture2DDesc::ChannelSwizzle::LuminanceAlpha;
    }

    auto backend_texture = m_backend->create_texture_2d(desc, nullptr, 0);
    if (!backend_texture || !backend_texture->valid()) {
        VITREO_TRACE("[d3d9] CreateTexture -> D3DERR_OUTOFVIDEOMEMORY "
                     "(shared backend rejected %ux%u mips=%u)\n",
                     width, height, levels);
        m.set_return(D3DERR_OUTOFVIDEOMEMORY);
        return;
    }

    // Metal private/shared textures start undefined; Windows RT contents are
    // unspecified until Clear/draw, but sampling uninit → neon green. Clear
    // RT textures to opaque black on create so empty lightmaps are dark.
    if (desc.render_target) {
        const std::array<float, 4> black{0.0f, 0.0f, 0.0f, 1.0f};
        (void)backend_texture->clear_render_target(black);
    }

    const u32 object = alloc_com_object(D3D9InterfaceId::Texture, device);
    if (object == 0) {
        m.set_return(E_OUTOFMEMORY_D3D);
        return;
    }
    ComObject* entry = find(object, D3D9InterfaceId::Texture);
    entry->texture = std::make_unique<TextureState>();
    TextureState& texture = *entry->texture;
    texture.width = width;
    texture.height = height;
    texture.levels = levels;
    texture.usage = usage;
    texture.format = format;
    texture.pool = pool;
    texture.backend = std::move(backend_texture);
    texture.level_surfaces.assign(levels, 0);

    if (m.write_ptr(out, object).is_err()) {
        destroy_object(object);
        m.set_return(E_POINTER_D3D);
        return;
    }
    VITREO_TRACE("[d3d9] CreateTexture -> texture=0x%08x %ux%u mips=%u "
                 "fmt=%u pool=%u usage=0x%x\n",
                 object, width, height, levels, format, pool, usage);
    if (d3d9_texture_lifetime_diag_enabled() &&
        is_large_texture(width, height)) {
        std::fprintf(stderr,
                     "[d3d9-texture-life] create texture=0x%08x %ux%u "
                     "levels=%u usage=0x%x format=%u pool=%u refs=%u\n",
                     object, width, height, levels, usage, format, pool,
                     entry->ref_count);
    }
    report_slow_resource_call("CreateTexture", create_started, width,
                              height);
    m.set_return(D3D_OK);
}

void D3D9Manager::create_cube_texture(Marshaler& m, u32 device) {
    // Learn: CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool,
    // ppCubeTexture, pSharedHandle).
    const u32 edge = static_cast<u32>(m.arg(1));
    u32 levels = static_cast<u32>(m.arg(2));
    const u32 usage = static_cast<u32>(m.arg(3));
    const u32 format = static_cast<u32>(m.arg(4));
    const u32 pool = static_cast<u32>(m.arg(5));
    const GuestAddr out = m.arg_ptr(6);
    const GuestAddr shared_handle = m.arg_ptr(7);

    if (out != 0) (void)m.write_u32(out, 0);
    auto finish = [&](u32 hr, const char* why) {
        std::fprintf(stderr,
                     "[d3d9] CreateCubeTexture edge=%u levels=%u usage=0x%x "
                     "fmt=%u pool=%u -> 0x%08x%s%s\n",
                     edge, levels, usage, format, pool, hr,
                     why != nullptr ? " " : "",
                     why != nullptr ? why : "");
        m.set_return(hr);
    };
    if (edge == 0 || out == 0) {
        finish(D3DERR_INVALIDCALL, "invalid edge/out");
        return;
    }
    if (shared_handle != 0) {
        finish(E_NOTIMPL_D3D, "shared handle");
        return;
    }

    const graphics::PixelFormat backend_format = d3d9_backend_format(format);
    u32 storage_pitch = 0;
    usize storage_bytes = 0;
    if (backend_format == graphics::PixelFormat::Unknown ||
        !d3d9_surface_storage(format, edge, edge, storage_pitch,
                              storage_bytes)) {
        finish(D3DERR_NOTAVAILABLE, "no shared-core format");
        return;
    }
    (void)storage_pitch;
    (void)storage_bytes;

    u32 max_levels = 1;
    for (u32 extent = edge; extent > 1; extent >>= 1) {
        ++max_levels;
    }
    if (levels == 0) levels = max_levels;
    if (levels > max_levels) {
        finish(D3DERR_INVALIDCALL, "levels");
        return;
    }

    if (m_backend == nullptr || !m_backend->available()) {
        finish(D3DERR_NOTAVAILABLE, "no backend");
        return;
    }

    graphics::BackendTexture2DDesc desc{};
    desc.width = edge;
    desc.height = edge;
    desc.mip_levels = levels;
    desc.array_size = 6;
    desc.cube = true;
    desc.pixel_format = backend_format;
    desc.shader_resource = true;
    desc.render_target = (usage & D3DUSAGE_RENDERTARGET) != 0;
    desc.cpu_writable = (usage & D3DUSAGE_DYNAMIC) != 0;

    auto backend_texture = m_backend->create_texture_2d(desc, nullptr, 0);
    if (!backend_texture || !backend_texture->valid()) {
        finish(D3DERR_OUTOFVIDEOMEMORY, "backend rejected");
        return;
    }

    const u32 object =
        alloc_com_object(D3D9InterfaceId::CubeTexture, device);
    if (object == 0) {
        finish(E_OUTOFMEMORY_D3D, "com alloc");
        return;
    }
    ComObject* entry = find(object, D3D9InterfaceId::CubeTexture);
    entry->texture = std::make_unique<TextureState>();
    TextureState& texture = *entry->texture;
    texture.width = edge;
    texture.height = edge;
    texture.levels = levels;
    texture.usage = usage;
    texture.format = format;
    texture.pool = pool;
    texture.cube = true;
    texture.backend = std::move(backend_texture);
    texture.level_surfaces.assign(D3DCUBE_FACE_COUNT * levels, 0);

    if (m.write_u32(out, object).is_err()) {
        destroy_object(object);
        finish(E_POINTER_D3D, "out pointer");
        return;
    }
    finish(D3D_OK, nullptr);
}

// =============================================================================
// IDirect3DVolumeTexture9 / IDirect3DVolume9
// =============================================================================

u32 D3D9Manager::create_volume_object(u32 device, u32 width, u32 height,
                                      u32 depth, u32 format, u32 usage,
                                      u32 pool, u32 container, u32 level) {
    const u32 object = alloc_com_object(D3D9InterfaceId::Volume, device);
    if (object == 0) return 0;

    ComObject* entry = find(object, D3D9InterfaceId::Volume);
    entry->volume = std::make_unique<VolumeState>();
    VolumeState& volume = *entry->volume;
    volume.width = width;
    volume.height = height;
    volume.depth = depth;
    volume.format = format;
    volume.usage = usage;
    volume.pool = pool;
    volume.container = container;
    volume.level = level;

    u32 row_pitch = 0;
    u32 slice_pitch = 0;
    usize bytes = 0;
    if (!d3d9_volume_storage(format, width, height, depth, row_pitch,
                             slice_pitch, bytes)) {
        VITREO_TRACE("[d3d9] volume: D3DFORMAT %u has no known pixel size\n",
                     format);
        destroy_object(object);
        return 0;
    }
    volume.row_pitch = row_pitch;
    volume.slice_pitch = slice_pitch;
    volume.pixel_bytes = bytes;
    volume.pixels = map_lock_region(volume.pixel_bytes);
    if (volume.pixels == 0) {
        VITREO_TRACE("[d3d9] volume: could not map %zu bytes of guest "
                     "pixel memory\n",
                     volume.pixel_bytes);
        destroy_object(object);
        return 0;
    }
    return object;
}

void D3D9Manager::create_volume_texture(Marshaler& m, u32 device) {
    // Learn: CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format,
    // Pool, ppVolumeTexture, pSharedHandle).
    const u32 width = static_cast<u32>(m.arg(1));
    const u32 height = static_cast<u32>(m.arg(2));
    const u32 depth = static_cast<u32>(m.arg(3));
    u32 levels = static_cast<u32>(m.arg(4));
    const u32 usage = static_cast<u32>(m.arg(5));
    const u32 format = static_cast<u32>(m.arg(6));
    const u32 pool = static_cast<u32>(m.arg(7));
    const GuestAddr out = m.arg_ptr(8);
    const GuestAddr shared_handle = m.arg_ptr(9);

    if (out != 0) (void)m.write_u32(out, 0);
    if (width == 0 || height == 0 || depth == 0 || out == 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (shared_handle != 0) {
        VITREO_TRACE("[d3d9] CreateVolumeTexture -> E_NOTIMPL "
                     "(no shared-resource handles)\n");
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
    if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED &&
        pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    const graphics::PixelFormat backend_format = d3d9_backend_format(format);
    u32 row_pitch = 0;
    u32 slice_pitch = 0;
    usize storage_bytes = 0;
    if (!d3d9_volume_storage(format, width, height, depth, row_pitch,
                             slice_pitch, storage_bytes)) {
        VITREO_TRACE("[d3d9] CreateVolumeTexture -> D3DERR_NOTAVAILABLE "
                     "(D3DFORMAT %u has no known volume storage)\n",
                     format);
        m.set_return(D3DERR_NOTAVAILABLE);
        return;
    }
    (void)row_pitch;
    (void)slice_pitch;
    (void)storage_bytes;

    u32 max_levels = 1;
    for (u32 extent = std::max(width, std::max(height, depth)); extent > 1;
         extent >>= 1) {
        ++max_levels;
    }
    if (levels == 0) levels = max_levels;
    if (levels > max_levels) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    // Learn: D3DPOOL_DEFAULT places the volume in video memory. Create a
    // shared-core 3D texture so LockBox/UnlockBox can upload voxels.
    std::unique_ptr<graphics::BackendTexture3D> backend_3d;
    if (pool == D3DPOOL_DEFAULT) {
        if (m_backend == nullptr || !m_backend->available()) {
            m.set_return(D3DERR_NOTAVAILABLE);
            return;
        }
        if (backend_format == graphics::PixelFormat::Unknown) {
            VITREO_TRACE("[d3d9] CreateVolumeTexture -> D3DERR_NOTAVAILABLE "
                         "(D3DFORMAT %u has no shared-core pixel format)\n",
                         format);
            m.set_return(D3DERR_NOTAVAILABLE);
            return;
        }
        graphics::BackendTexture3DDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.depth = depth;
        desc.mip_levels = levels;
        desc.pixel_format = backend_format;
        desc.shader_resource = true;
        desc.cpu_writable = (usage & D3DUSAGE_DYNAMIC) != 0;
        backend_3d = m_backend->create_texture_3d(desc, nullptr, 0, 0);
        if (!backend_3d || !backend_3d->valid()) {
            VITREO_TRACE("[d3d9] CreateVolumeTexture -> D3DERR_OUTOFVIDEOMEMORY "
                         "(shared backend rejected %ux%ux%u mips=%u)\n",
                         width, height, depth, levels);
            m.set_return(D3DERR_OUTOFVIDEOMEMORY);
            return;
        }
    }

    const u32 object =
        alloc_com_object(D3D9InterfaceId::VolumeTexture, device);
    if (object == 0) {
        m.set_return(E_OUTOFMEMORY_D3D);
        return;
    }
    ComObject* entry = find(object, D3D9InterfaceId::VolumeTexture);
    entry->texture = std::make_unique<TextureState>();
    TextureState& texture = *entry->texture;
    texture.width = width;
    texture.height = height;
    texture.depth = depth;
    texture.levels = levels;
    texture.usage = usage;
    texture.format = format;
    texture.pool = pool;
    texture.volume = true;
    texture.backend_3d = std::move(backend_3d);
    texture.level_surfaces.assign(levels, 0);

    if (m.write_u32(out, object).is_err()) {
        destroy_object(object);
        m.set_return(E_POINTER_D3D);
        return;
    }
    VITREO_TRACE("[d3d9] CreateVolumeTexture -> volume=0x%08x %ux%ux%u "
                 "mips=%u fmt=%u pool=%u usage=0x%x\n",
                 object, width, height, depth, levels, format, pool, usage);
    m.set_return(D3D_OK);
}

} // namespace vitreo::winapi
