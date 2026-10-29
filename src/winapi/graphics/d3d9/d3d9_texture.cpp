// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_texture.cpp
/// @brief IDirect3DTexture9 dispatch.

#include "winapi/graphics/d3d9/d3d9_resources_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/host_gui_pump.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace vitreo::winapi {

using namespace d3d9_resources_detail;

// =============================================================================
// IDirect3DTexture9
// =============================================================================

void D3D9Manager::dispatch_texture(Marshaler& m, usize slot, u32 self) {
    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::Texture)) return;
    if (dispatch_resource_base(m, slot, self, D3DRTYPE_TEXTURE)) return;

    ComObject* entry = find(self, D3D9InterfaceId::Texture);
    if (entry == nullptr || !entry->texture) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    TextureState& texture = *entry->texture;

    // Resolve or create the surface object for a level, so repeated
    // GetSurfaceLevel calls return the same interface pointer as Windows does.
    const auto level_surface = [&](u32 level) -> u32 {
        if (level >= texture.levels) return 0;
        if (texture.level_surfaces[level] != 0) {
            return texture.level_surfaces[level];
        }
        u32 level_width = texture.width >> level;
        u32 level_height = texture.height >> level;
        if (level_width == 0) level_width = 1;
        if (level_height == 0) level_height = 1;
        const u32 surface = create_surface_object(
            entry->device, level_width, level_height, texture.format,
            texture.usage, texture.pool,
            static_cast<int>(SurfaceState::Kind::TextureLevel), self, level);
        if (surface != 0) texture.level_surfaces[level] = surface;
        return surface;
    };

    switch (slot) {
    case 11: { // SetLOD (returns the previous LOD)
        const u32 previous = texture.lod;
        // Only a managed texture has a settable LOD; Windows returns 0 for
        // any other pool without changing anything.
        if (texture.pool == D3DPOOL_MANAGED) {
            texture.lod = std::min(static_cast<u32>(m.arg(1)),
                                   texture.levels - 1);
            m.set_return(previous);
            return;
        }
        m.set_return(0);
        return;
    }
    case 12: // GetLOD
        m.set_return(texture.pool == D3DPOOL_MANAGED ? texture.lod : 0);
        return;
    case 13: // GetLevelCount
        m.set_return(texture.levels);
        return;
    case 14: { // SetAutoGenFilterType
        // Learn: without D3DUSAGE_AUTOGENMIPMAP the call has no effect and
        // does not fail. With it, NONE / unsupported filters → INVALIDCALL.
        const u32 filter = static_cast<u32>(m.arg(1));
        if ((texture.usage & D3DUSAGE_AUTOGENMIPMAP) == 0) {
            m.set_return(D3D_OK);
            return;
        }
        if (!is_supported_autogen_filter(filter)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const bool dirty = texture.auto_gen_filter != filter;
        texture.auto_gen_filter = filter;
        if (dirty) {
            // Learn: changing the filter dirties and regenerates sublevels.
            generate_autogen_mips(m, texture, entry->device, self);
        }
        m.set_return(D3D_OK);
        return;
    }
    case 15: // GetAutoGenFilterType
        m.set_return(texture.auto_gen_filter);
        return;
    case 16: { // GenerateMipSubLevels (void return)
        if (texture.levels > 1) {
            generate_autogen_mips(m, texture, entry->device, self);
        }
        m.set_return(0);
        return;
    }
    case 17: { // GetLevelDesc
        const u32 level = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        const u32 surface = level_surface(level);
        ComObject* surface_entry =
            surface != 0 ? find(surface, D3D9InterfaceId::Surface) : nullptr;
        if (out == 0 || surface_entry == nullptr || !surface_entry->surface) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u32, 8> desc{};
        write_surface_desc(desc, *surface_entry->surface);
        if (m.memory().write_bytes(
                out, reinterpret_cast<const Byte*>(desc.data()),
                D3DSURFACE_DESC_SIZE).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (std::getenv("VITREO_D3D9_DRAW_DIAG") != nullptr &&
            surface_entry->surface->height > 0 &&
            surface_entry->surface->height <= 64 &&
            surface_entry->surface->width <= 256) {
            static std::unordered_set<u64> seen_level_desc;
            const u64 key =
                (static_cast<u64>(self) << 32) |
                (static_cast<u64>(level) << 16) |
                surface_entry->surface->width;
            if (seen_level_desc.size() < 64 &&
                seen_level_desc.insert(key).second) {
                std::fprintf(stderr,
                             "[d3d9-leveldesc] texture=0x%08x level=%u "
                             "%ux%u fmt=%u\n",
                             self, level, surface_entry->surface->width,
                             surface_entry->surface->height,
                             surface_entry->surface->format);
            }
        }
        m.set_return(D3D_OK);
        return;
    }
    case 18: { // GetSurfaceLevel
        const u32 level = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_u32(out, 0);
        const u32 surface = level_surface(level);
        if (out == 0 || surface == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(surface);
        if (d3d9_texture_lifetime_diag_enabled() &&
            is_large_texture(texture.width, texture.height)) {
            const ComObject* level_entry =
                find(surface, D3D9InterfaceId::Surface);
            std::fprintf(stderr,
                         "[d3d9-texture-life] get-level texture=0x%08x "
                         "surface=0x%08x level=%u surface_refs=%u\n",
                         self, surface, level,
                         level_entry != nullptr ? level_entry->ref_count : 0u);
        }
        if (m.write_u32(out, surface).is_err()) {
            (void)release(surface);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 19: { // LockRect
        const u32 level = static_cast<u32>(m.arg(1));
        const GuestAddr locked_rect_out = m.arg_ptr(2);
        const GuestAddr rect_in = m.arg_ptr(3);
        const u32 flags = static_cast<u32>(m.arg(4));
        const u32 surface = level_surface(level);
        ComObject* surface_entry =
            surface != 0 ? find(surface, D3D9InterfaceId::Surface) : nullptr;
        if (locked_rect_out == 0 || surface_entry == nullptr ||
            !surface_entry->surface) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        SurfaceState& level_state = *surface_entry->surface;
        if (level_state.locked || level_state.pixels == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        d3d9_cs_drain();
        GuestAddr locked_bits = 0;
        if (!locked_rect_view(m, level_state, rect_in, flags, locked_bits)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m.write_u32(locked_rect_out, level_state.pitch).is_err() ||
            m.write_ptr(locked_rect_out + (m.cpu_is_64bit() ? 8 : 4), locked_bits).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (level_state.width >= 256u && level_state.height >= 256u) {
            pump_host_gui();
        }
        level_state.locked = true;
        level_state.lock_flags = flags;
        static u32 lock_ok = 0;
        ++lock_ok;
        if (lock_ok == 1 || (lock_ok & 0xFFu) == 0) {
            VITREO_TRACE("[d3d9] IDirect3DTexture9::LockRect ok #%u "
                         "%ux%u level=%u flags=0x%x\n",
                         lock_ok, level_state.width, level_state.height, level,
                         flags);
        }
        m.set_return(D3D_OK);
        return;
    }
    case 20: { // UnlockRect
        const auto unlock_started = std::chrono::steady_clock::now();
        const u32 level = static_cast<u32>(m.arg(1));
        if (level >= texture.levels ||
            texture.level_surfaces[level] == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        ComObject* surface_entry =
            find(texture.level_surfaces[level], D3D9InterfaceId::Surface);
        if (surface_entry == nullptr || !surface_entry->surface ||
            !surface_entry->surface->locked) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        SurfaceState& level_state = *surface_entry->surface;
        level_state.locked = false;
        // Same MANAGED/SYSTEMMEM READONLY caveat as IDirect3DSurface9::UnlockRect.
        const bool cpu_owned_pool =
            texture.pool == D3DPOOL_MANAGED ||
            texture.pool == D3DPOOL_SYSTEMMEM;
        const bool allow_upload =
            (level_state.lock_flags & D3DLOCK_READONLY) == 0 || cpu_owned_pool;
        if (allow_upload && texture.backend) {
            if (level_state.width >= 256u && level_state.height >= 256u) {
                pump_host_gui();
            }
            std::vector<u8> pixels(level_state.pixel_bytes);
            std::vector<u8> expanded;
            if (m.memory()
                    .read_bytes(level_state.pixels, pixels.data(),
                                pixels.size())
                    .is_ok()) {
                const u8* upload = pixels.data();
                u32 upload_pitch = level_state.pitch;
                const bool native_bgra =
                    texture.format == 21u /* A8R8G8B8 */ ||
                    texture.format == 22u /* X8R8G8B8 */;
                if (d3d9_is_palettized_format(texture.format)) {
                    DeviceState* device_state = nullptr;
                    if (ComObject* device_entry =
                            find(entry->device, D3D9InterfaceId::Device);
                        device_entry != nullptr &&
                        device_entry->device_state) {
                        device_state = device_entry->device_state.get();
                    }
                    const u8* palette =
                        device_state != nullptr
                            ? resolve_current_palette_entries(*device_state)
                            : nullptr;
                    if (d3d9_expand_palettized_to_bgra8(
                            texture.format, level_state.width,
                            level_state.height, upload,
                            level_state.pitch, palette, expanded)) {
                        upload = expanded.data();
                        upload_pitch = level_state.width * 4u;
                        if (device_state != nullptr) {
                            texture.palettized_upload_serial =
                                device_state->texture_palette_serial;
                        }
                    }
                } else if (!native_bgra) {
                    const bool expanded_bump =
                        d3d9_expand_special_bump_to_rgba32f(
                            texture.format, level_state.width,
                            level_state.height, upload, level_state.pitch,
                            expanded);
                    if (expanded_bump || d3d9_expand_lock_to_bgra8(
                            texture.format, level_state.width,
                            level_state.height, upload, level_state.pitch,
                            expanded)) {
                        upload = expanded.data();
                        upload_pitch = level_state.width *
                                       (expanded_bump ? 16u : 4u);
                    }
                }
                if (!texture.backend->update(level, upload, upload_pitch)) {
                    VITREO_TRACE("[d3d9] UnlockRect: backend rejected the "
                                 "upload of level %u\n", level);
                    m.set_return(D3DERR_INVALIDCALL);
                    return;
                }
                if (d3d9_texture_lifetime_diag_enabled() &&
                    is_large_texture(texture.width, texture.height)) {
                    std::fprintf(stderr,
                                 "[d3d9-texture-life] upload "
                                 "texture=0x%08x %ux%u level=%u pitch=%u\n",
                                 self, level_state.width, level_state.height,
                                 level, upload_pitch);
                }
                // Bring-up probe: is the guest handing us real pixels, or a
                // blank surface? Distinguishes a broken guest-side image
                // decode from a rendering fault.
                static const bool upload_content_diag =
                    std::getenv("VITREO_D3D9_UPLOAD_CONTENT") != nullptr;
                if (upload_content_diag) {
                    const usize bytes =
                        static_cast<usize>(upload_pitch) * level_state.height;
                    usize nonzero = 0;
                    u64 hash = 0xcbf29ce484222325ull;
                    for (usize i = 0; i < bytes; ++i) {
                        if (upload[i] != 0) ++nonzero;
                        hash = (hash ^ upload[i]) * 0x100000001b3ull;
                    }
                    std::fprintf(stderr,
                                 "[d3d9-upload] texture=0x%08x %ux%u fmt=%u "
                                 "bytes=%zu nonzero=%zu hash=0x%llx "
                                 "head=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                                 self, level_state.width, level_state.height,
                                 level_state.format, bytes, nonzero,
                                 static_cast<unsigned long long>(hash),
                                 bytes > 0 ? upload[0] : 0,
                                 bytes > 1 ? upload[1] : 0,
                                 bytes > 2 ? upload[2] : 0,
                                 bytes > 3 ? upload[3] : 0,
                                 bytes > 4 ? upload[4] : 0,
                                 bytes > 5 ? upload[5] : 0,
                                 bytes > 6 ? upload[6] : 0,
                                 bytes > 7 ? upload[7] : 0);
                    // Bring-up: A8R8G8B8 UI atlases that upload as white with
                    // A=0 at the origin may still be fully transparent — which
                    // SRCALPHA menu draws would not show. Count alpha and
                    // optionally dump a PPM of RGB + alpha-as-gray.
                    if (level_state.format == 21u &&
                        level_state.width >= 64u &&
                        level_state.height >= 32u &&
                        upload_pitch >= level_state.width * 4u) {
                        usize a_nonzero = 0;
                        usize a_sum = 0;
                        u8 a_min = 255;
                        u8 a_max = 0;
                        for (u32 y = 0; y < level_state.height; ++y) {
                            const u8* row =
                                upload +
                                static_cast<usize>(y) * upload_pitch;
                            for (u32 x = 0; x < level_state.width; ++x) {
                                const u8 a = row[x * 4u + 3u];
                                if (a != 0) ++a_nonzero;
                                a_sum += a;
                                if (a < a_min) a_min = a;
                                if (a > a_max) a_max = a;
                            }
                        }
                        std::fprintf(stderr,
                                     "[d3d9-upload-alpha] texture=0x%08x "
                                     "%ux%u a_nonzero=%zu/%u a_min=%u "
                                     "a_max=%u a_mean=%u\n",
                                     self, level_state.width,
                                     level_state.height, a_nonzero,
                                     level_state.width * level_state.height,
                                     a_min, a_max,
                                     static_cast<unsigned>(
                                         a_sum /
                                         std::max<usize>(
                                             1, static_cast<usize>(
                                                    level_state.width) *
                                                    level_state.height)));
                        static const char* dump_dir =
                            std::getenv("VITREO_D3D9_DUMP_UPLOAD");
                        static u32 dumps = 0;
                        if (dump_dir != nullptr && dumps < 6u) {
                            char path[512];
                            std::snprintf(path, sizeof(path),
                                          "%s/tex_%08x_%ux%u.ppm", dump_dir,
                                          self, level_state.width,
                                          level_state.height);
                            if (FILE* fp = std::fopen(path, "wb")) {
                                // Side-by-side RGB | alpha-as-luma.
                                const u32 out_w = level_state.width * 2u;
                                std::fprintf(fp, "P6\n%u %u\n255\n", out_w,
                                             level_state.height);
                                std::vector<u8> row(static_cast<usize>(out_w) *
                                                    3u);
                                for (u32 y = 0; y < level_state.height; ++y) {
                                    const u8* src =
                                        upload +
                                        static_cast<usize>(y) * upload_pitch;
                                    for (u32 x = 0; x < level_state.width;
                                         ++x) {
                                        const u8 b = src[x * 4u + 0u];
                                        const u8 g = src[x * 4u + 1u];
                                        const u8 r = src[x * 4u + 2u];
                                        const u8 a = src[x * 4u + 3u];
                                        const usize o =
                                            static_cast<usize>(x) * 3u;
                                        row[o + 0] = r;
                                        row[o + 1] = g;
                                        row[o + 2] = b;
                                        const usize oa =
                                            static_cast<usize>(
                                                x + level_state.width) *
                                            3u;
                                        row[oa + 0] = a;
                                        row[oa + 1] = a;
                                        row[oa + 2] = a;
                                    }
                                    std::fwrite(row.data(), 1, row.size(),
                                                fp);
                                }
                                std::fclose(fp);
                                ++dumps;
                                std::fprintf(stderr,
                                             "[d3d9-upload-dump] wrote %s\n",
                                             path);
                            }
                        }
                    }
                }
                static u32 unlock_ok = 0;
                ++unlock_ok;
                if (unlock_ok == 1 || (unlock_ok & 0xFFu) == 0 ||
                    level_state.format == 50u || level_state.format == 51u) {
                    VITREO_TRACE("[d3d9] IDirect3DTexture9::UnlockRect upload "
                                 "ok #%u %ux%u level=%u fmt=%u\n",
                                 unlock_ok, level_state.width,
                                 level_state.height, level,
                                 level_state.format);
                }
                if (level == 0 &&
                    (texture.usage & D3DUSAGE_AUTOGENMIPMAP) != 0 &&
                    texture.levels > 1) {
                    generate_autogen_mips(m, texture, entry->device, self);
                }
            }
        }
        report_slow_resource_call("texture UnlockRect", unlock_started,
                                  level_state.width, level_state.height);
        m.set_return(D3D_OK);
        return;
    }
    case 21: // AddDirtyRect
        // Vitreo uploads the whole level on unlock, so the dirty region is
        // already covered; recording it would change nothing observable.
        m.set_return(D3D_OK);
        return;
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

} // namespace vitreo::winapi
