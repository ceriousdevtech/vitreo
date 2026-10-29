// SPDX-License-Identifier: LGPL-2.1-only

/// @file ddraw_surface_pixels.cpp
/// @brief IDirectDrawSurface pixel access and transfer: Lock, Unlock, Blt and
///        BltFast.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "graphics/contracts/cpu_surface.hpp"
#include "graphics/common/perf_diag.hpp"
#include "core/memory/guest_memory.hpp"

#include <array>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

u32 DirectDrawManager::surface_lock(GuestAddr surface, GuestAddr rectangle,
                                     GuestAddr descriptor, u32 flags,
                                     GuestAddr event) {
    SurfaceObject* state = find_surface(surface);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    // hEvent is reserved and must be NULL on Windows.
    if (!m_memory || descriptor == 0 || event != 0) {
        return ddraw::DDERR_INVALIDPARAMS;
    }
    if (m_memory->read_u32(descriptor).value_or(0) != 108) {
        return ddraw::DDERR_INVALIDPARAMS;
    }
    if (state->locked) return ddraw::DDERR_SURFACEBUSY;
    const u32 width = state->surface->width();
    const u32 height = state->surface->height();
    const u32 pitch = state->surface->pitch();
    const u32 bpp = state->surface->bits_per_pixel();
    if (bpp == 0 || (bpp % 8) != 0) {
        return ddraw::E_NOTIMPL;
    }
    const u32 bytes_per_pixel = bpp / 8;

    u32 left = 0;
    u32 top = 0;
    u32 right = width;
    u32 bottom = height;
    if (rectangle != 0) {
        // RECT { LONG left, top, right, bottom }. A non-NULL rect selects a
        // sub-rectangle: lpSurface points at its top-left, pitch stays the
        // full surface pitch. A partial update uses this. Refusing it leaves
        // the primary black except the strip a later full-surface Lock can
        // still reach.
        const i32 r_left = static_cast<i32>(
            m_memory->read_u32(rectangle + 0).value_or(0));
        const i32 r_top = static_cast<i32>(
            m_memory->read_u32(rectangle + 4).value_or(0));
        const i32 r_right = static_cast<i32>(
            m_memory->read_u32(rectangle + 8).value_or(0));
        const i32 r_bottom = static_cast<i32>(
            m_memory->read_u32(rectangle + 12).value_or(0));
        if (r_left < 0 || r_top < 0 || r_right <= r_left || r_bottom <= r_top ||
            static_cast<u32>(r_right) > width ||
            static_cast<u32>(r_bottom) > height) {
            return ddraw::E_INVALIDARG;
        }
        left = static_cast<u32>(r_left);
        top = static_cast<u32>(r_top);
        right = static_cast<u32>(r_right);
        bottom = static_cast<u32>(r_bottom);
    }

    const GuestAddr locked_ptr =
        state->pixels + static_cast<GuestAddr>(top) * pitch +
        static_cast<GuestAddr>(left) * bytes_per_pixel;

    constexpr u32 output_flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x800 | 0x1000;
    (void)m_memory->write_u32(descriptor + 4, output_flags);
    // Report the locked region size; pitch remains the full surface pitch so
    // row advances stay correct for sub-rectangle locks.
    (void)m_memory->write_u32(descriptor + 8, bottom - top);
    (void)m_memory->write_u32(descriptor + 12, right - left);
    (void)m_memory->write_u32(descriptor + 16, pitch);
    (void)m_memory->write_u32(descriptor + 36, static_cast<u32>(locked_ptr));

    // Lock sets DDSD_PIXELFORMAT, so the whole DDPIXELFORMAT must describe this
    // surface. Reporting DDPF_PALETTEINDEXED8 unconditionally told a 16- or
    // 32-bit caller its surface was paletted, and left the channel masks
    // holding whatever the caller's buffer had before the call.
    constexpr u32 DDPF_RGB = 0x00000040;
    constexpr u32 DDPF_PALETTEINDEXED8 = 0x00000020;
    for (u32 offset = 72; offset < 104; offset += 4) {
        (void)m_memory->write_u32(descriptor + offset, 0);
    }
    (void)m_memory->write_u32(descriptor + 72, 32);  // DDPIXELFORMAT dwSize
    (void)m_memory->write_u32(descriptor + 76,
                              bpp == 8 ? DDPF_PALETTEINDEXED8 : DDPF_RGB);
    (void)m_memory->write_u32(descriptor + 84, bpp);
    if (bpp == 16) {
        (void)m_memory->write_u32(descriptor + 88, 0xF800);
        (void)m_memory->write_u32(descriptor + 92, 0x07E0);
        (void)m_memory->write_u32(descriptor + 96, 0x001F);
    } else if (bpp == 24 || bpp == 32) {
        (void)m_memory->write_u32(descriptor + 88, 0x00FF0000);
        (void)m_memory->write_u32(descriptor + 92, 0x0000FF00);
        (void)m_memory->write_u32(descriptor + 96, 0x000000FF);
    }
    (void)m_memory->write_u32(descriptor + 104, state->caps);
    state->locked = true;
    state->locked_ptr = locked_ptr;
    perf::count(perf::Counter::dd_lock);
    VITREO_TRACE("[ddraw] Surface::Lock surface=0x%llx flags=0x%x rect=%s "
                 "-> pixels=0x%llx\n",
                 static_cast<unsigned long long>(surface), flags,
                 rectangle != 0 ? "yes" : "no",
                 static_cast<unsigned long long>(locked_ptr));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_unlock(GuestAddr surface, GuestAddr pixels) {
    SurfaceObject* state = find_surface(surface);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (!state->locked) return ddraw::E_INVALIDARG;
    if (pixels != 0) {
        const GuestAddr begin = state->pixels;
        const GuestAddr end = state->pixels + state->pixel_bytes;
        if (pixels < begin || pixels >= end) return ddraw::E_INVALIDARG;
    }
    state->locked = false;
    state->locked_ptr = 0;
    perf::count(perf::Counter::dd_unlock);
    // DDSCAPS_PRIMARYSURFACE. Separating primary from offscreen unlocks is what
    // shows whether the guest is compositing into a back buffer (cheap) or
    // driving the display directly on every unlock (a present per blit).
    if ((state->caps & 0x200u) != 0) {
        perf::count(perf::Counter::dd_unlock_primary);
    }
    state->surface->mark_dirty();
    present_surface(surface, *state);
    VITREO_TRACE("[ddraw] Surface::Unlock surface=0x%llx generation=%llu\n",
                 static_cast<unsigned long long>(surface),
                 static_cast<unsigned long long>(state->surface->generation()));
    return ddraw::DD_OK;
}

namespace {

[[nodiscard]] bool read_rect(GuestMemory& memory, GuestAddr rect, i32& left,
                             i32& top, i32& right, i32& bottom) noexcept {
    left = static_cast<i32>(memory.read_u32(rect + 0).value_or(0));
    top = static_cast<i32>(memory.read_u32(rect + 4).value_or(0));
    right = static_cast<i32>(memory.read_u32(rect + 8).value_or(0));
    bottom = static_cast<i32>(memory.read_u32(rect + 12).value_or(0));
    return right > left && bottom > top;
}

} // namespace

u32 DirectDrawManager::surface_blt(GuestAddr dest, GuestAddr dest_rect,
                                   GuestAddr src, GuestAddr src_rect, u32 flags,
                                   GuestAddr fx) {
    // learn.microsoft.com: IDirectDrawSurface::Blt — software path for
    // DDBLT_WAIT copies, DDBLT_COLORFILL, and 8-bit DDBLT_KEYSRC. Stretch,
    // alpha, z, and hardware FX remain E_NOTIMPL.
    constexpr u32 DDBLT_COLORFILL = 0x00000400;
    constexpr u32 DDBLT_KEYSRC = 0x00008000;
    constexpr u32 DDBLT_KEYDEST = 0x00002000;
    constexpr u32 DDBLT_ROP = 0x00020000;
    constexpr u32 DDBLT_WAIT = 0x01000000;
    constexpr u32 DDBLT_ASYNC = 0x00000200;
    (void)DDBLT_WAIT;
    (void)DDBLT_ASYNC;

    SurfaceObject* dest_state = find_surface(dest);
    if (!dest_state) return ddraw::DDERR_INVALIDOBJECT;
    if (!m_memory) return ddraw::DDERR_INVALIDPARAMS;

    const u32 unsupported =
        flags & ~(DDBLT_WAIT | DDBLT_ASYNC | DDBLT_COLORFILL | DDBLT_KEYSRC);
    if ((unsupported & (DDBLT_KEYDEST | DDBLT_ROP)) != 0) {
        VITREO_TRACE("[ddraw] Blt unsupported flags=0x%x\n", flags);
        return ddraw::E_NOTIMPL;
    }

    const u32 dest_w = dest_state->surface->width();
    const u32 dest_h = dest_state->surface->height();
    const u32 dest_pitch = dest_state->surface->pitch();
    const u32 bpp = dest_state->surface->bits_per_pixel();
    if (bpp == 0 || (bpp % 8) != 0) return ddraw::E_NOTIMPL;
    const u32 bpp_bytes = bpp / 8;

    i32 dl = 0, dt = 0, dr = static_cast<i32>(dest_w), db = static_cast<i32>(dest_h);
    if (dest_rect != 0) {
        if (!read_rect(*m_memory, dest_rect, dl, dt, dr, db) || dl < 0 ||
            dt < 0 || static_cast<u32>(dr) > dest_w ||
            static_cast<u32>(db) > dest_h) {
            return ddraw::E_INVALIDARG;
        }
    }
    const u32 out_w = static_cast<u32>(dr - dl);
    const u32 out_h = static_cast<u32>(db - dt);

    if (dest_state->locked) {
        VITREO_TRACE("[ddraw] Blt dest=0x%llx src=0x%llx flags=0x%x "
                     "-> DDERR_SURFACEBUSY (destination region locked)\n",
                     static_cast<unsigned long long>(dest),
                     static_cast<unsigned long long>(src), flags);
        return ddraw::DDERR_SURFACEBUSY;
    }

    Byte* dest_host =
        m_memory->host_ptr_mut(dest_state->pixels, dest_state->pixel_bytes);
    if (!dest_host) return 0x8007000Eu;

    // A clipper attached to the destination confines the blit to its region.
    // The transfer is 1:1, so each surviving sub-rectangle shifts the source
    // origin by exactly the delta its destination origin moved.
    std::vector<std::array<i32, 4>> pieces;
    if (dest_state->clipper != 0) {
        ClipperObject* clip_state = find_clipper(dest_state->clipper);
        std::vector<std::array<i32, 4>> region;
        if (clip_state == nullptr || !clipper_region(*clip_state, region)) {
            // A clipper with no region cannot say where the blit may land.
            return ddraw::DDERR_NOCLIPLIST;
        }
        for (const auto& r : region) {
            const i32 left = r[0] > dl ? r[0] : dl;
            const i32 top = r[1] > dt ? r[1] : dt;
            const i32 right = r[2] < dr ? r[2] : dr;
            const i32 bottom = r[3] < db ? r[3] : db;
            if (right > left && bottom > top) {
                pieces.push_back({left, top, right, bottom});
            }
        }
        // A blit entirely outside the clip region draws nothing; that is a
        // successful no-op, not a failure.
        if (pieces.empty()) return ddraw::DD_OK;
    } else {
        pieces.push_back({dl, dt, dr, db});
    }

    i32 sl = 0;
    i32 st = 0;
    i32 sr = 0;
    i32 sb = 0;
    if ((flags & DDBLT_COLORFILL) != 0) {
        if (fx == 0) return ddraw::E_INVALIDARG;
        // DDBLTFX.dwFillColor at offset 0x50 in the classic structure.
        // Callers place it there after dwSize.
        const u32 fill = m_memory->read_u32(fx + 0x50).value_or(0);
        for (const auto& piece : pieces) {
            const u32 piece_w = static_cast<u32>(piece[2] - piece[0]);
            const u32 piece_h = static_cast<u32>(piece[3] - piece[1]);
            for (u32 y = 0; y < piece_h; ++y) {
                Byte* row =
                    dest_host +
                    static_cast<usize>(
                        (static_cast<u32>(piece[1]) + y) * dest_pitch +
                        static_cast<u32>(piece[0]) * bpp_bytes);
                if (bpp_bytes == 1) {
                    std::memset(row, static_cast<int>(fill & 0xFF), piece_w);
                } else {
                    for (u32 x = 0; x < piece_w; ++x) {
                        for (u32 b = 0; b < bpp_bytes; ++b) {
                            row[x * bpp_bytes + b] =
                                static_cast<Byte>((fill >> (8 * b)) & 0xFF);
                        }
                    }
                }
            }
        }
    } else {
        if (src == 0) return ddraw::E_INVALIDARG;
        SurfaceObject* src_state = find_surface(src);
        if (!src_state) return ddraw::DDERR_INVALIDOBJECT;
        if (src_state->surface->bits_per_pixel() != bpp) {
            return ddraw::E_NOTIMPL;
        }
        const bool key_src = (flags & DDBLT_KEYSRC) != 0;
        if (key_src && !src_state->has_src_color_key) {
            return ddraw::E_INVALIDARG;
        }
        if (key_src && bpp_bytes != 1) {
            // Only 8-bit source keys are implemented; RGB keys stay E_NOTIMPL.
            return ddraw::E_NOTIMPL;
        }
        const u32 src_w = src_state->surface->width();
        const u32 src_h = src_state->surface->height();
        const u32 src_pitch = src_state->surface->pitch();
        sr = static_cast<i32>(src_w);
        sb = static_cast<i32>(src_h);
        if (src_rect != 0) {
            if (!read_rect(*m_memory, src_rect, sl, st, sr, sb) || sl < 0 ||
                st < 0 || static_cast<u32>(sr) > src_w ||
                static_cast<u32>(sb) > src_h) {
                return ddraw::E_INVALIDARG;
            }
        }
        if (src_state->locked) {
            VITREO_TRACE("[ddraw] Blt dest=0x%llx src=0x%llx flags=0x%x "
                         "-> DDERR_SURFACEBUSY (source region locked)\n",
                         static_cast<unsigned long long>(dest),
                         static_cast<unsigned long long>(src), flags);
            return ddraw::DDERR_SURFACEBUSY;
        }
        const u32 in_w = static_cast<u32>(sr - sl);
        const u32 in_h = static_cast<u32>(sb - st);
        // No stretch yet. Callers that save and restore a dialog use equal RECTs.
        if (in_w != out_w || in_h != out_h) {
            VITREO_TRACE("[ddraw] Blt stretch %ux%u -> %ux%u not implemented\n",
                         in_w, in_h, out_w, out_h);
            return ddraw::E_NOTIMPL;
        }
        const Byte* src_host =
            m_memory->host_ptr(src_state->pixels, src_state->pixel_bytes);
        if (!src_host) return 0x8007000Eu;

        const bool same = dest_state == src_state;
        const u32 key_lo = src_state->src_color_key_low & 0xFFu;
        const u32 key_hi = src_state->src_color_key_high & 0xFFu;
        for (const auto& piece : pieces) {
            const u32 piece_w = static_cast<u32>(piece[2] - piece[0]);
            const u32 piece_h = static_cast<u32>(piece[3] - piece[1]);
            // The clipped destination origin drags the source origin with it.
            const i32 piece_sl = sl + (piece[0] - dl);
            const i32 piece_st = st + (piece[1] - dt);
            const usize row_bytes = static_cast<usize>(piece_w) * bpp_bytes;
            const auto copy_row = [&](u32 y) {
                Byte* drow =
                    dest_host +
                    static_cast<usize>(
                        (static_cast<u32>(piece[1]) + y) * dest_pitch +
                        static_cast<u32>(piece[0]) * bpp_bytes);
                const Byte* srow =
                    src_host +
                    static_cast<usize>(
                        (static_cast<u32>(piece_st) + y) * src_pitch +
                        static_cast<u32>(piece_sl) * bpp_bytes);
                if (key_src) {
                    for (u32 x = 0; x < piece_w; ++x) {
                        const u8 px = srow[x];
                        if (px >= key_lo && px <= key_hi) continue;
                        drow[x] = px;
                    }
                } else if (same) {
                    std::memmove(drow, srow, row_bytes);
                } else {
                    std::memcpy(drow, srow, row_bytes);
                }
            };
            // Overlapping copies within one surface must run away from the
            // direction of travel so rows are read before being overwritten.
            if (same && static_cast<u32>(piece[1]) >
                            static_cast<u32>(piece_st)) {
                for (u32 y = piece_h; y > 0; --y) copy_row(y - 1);
            } else {
                for (u32 y = 0; y < piece_h; ++y) copy_row(y);
            }
        }
    }

    dest_state->surface->mark_dirty();
    present_surface(dest, *dest_state);
    VITREO_TRACE("[ddraw] Blt dest=0x%llx rect=(%d,%d)-(%d,%d) "
                 "src=0x%llx rect=(%d,%d)-(%d,%d) flags=0x%x %ux%u\n",
                 static_cast<unsigned long long>(dest),
                 dl, dt, dr, db, static_cast<unsigned long long>(src),
                 sl, st, sr, sb, flags, out_w, out_h);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_blt_fast(GuestAddr dest, u32 dest_x, u32 dest_y,
                                        GuestAddr src, GuestAddr src_rect,
                                        u32 flags) {
    // learn.microsoft.com: IDirectDrawSurface::BltFast — copy or keyed blit to
    // a destination (x,y). Implemented as a dest-RECT wrapper around Blt.
    constexpr u32 DDBLTFAST_SRCCOLORKEY = 0x00000001;
    constexpr u32 DDBLTFAST_DESTCOLORKEY = 0x00000002;
    constexpr u32 DDBLTFAST_WAIT = 0x00000010;
    constexpr u32 DDBLTFAST_NOCOLORKEY = 0x00000000;
    (void)DDBLTFAST_NOCOLORKEY;

    if ((flags & DDBLTFAST_DESTCOLORKEY) != 0) return ddraw::E_NOTIMPL;

    SurfaceObject* dest_state = find_surface(dest);
    if (!dest_state) return ddraw::DDERR_INVALIDOBJECT;
    if (!m_memory) return ddraw::DDERR_INVALIDPARAMS;
    if (src == 0) return ddraw::E_INVALIDARG;
    SurfaceObject* src_state = find_surface(src);
    if (!src_state) return ddraw::DDERR_INVALIDOBJECT;

    const u32 dest_w = dest_state->surface->width();
    const u32 dest_h = dest_state->surface->height();
    const u32 src_w = src_state->surface->width();
    const u32 src_h = src_state->surface->height();

    i32 sl = 0, st = 0, sr = static_cast<i32>(src_w), sb = static_cast<i32>(src_h);
    if (src_rect != 0) {
        if (!read_rect(*m_memory, src_rect, sl, st, sr, sb) || sl < 0 ||
            st < 0 || static_cast<u32>(sr) > src_w ||
            static_cast<u32>(sb) > src_h) {
            return ddraw::E_INVALIDARG;
        }
    }
    const u32 copy_w = static_cast<u32>(sr - sl);
    const u32 copy_h = static_cast<u32>(sb - st);
    if (dest_x > dest_w || dest_y > dest_h ||
        dest_x + copy_w > dest_w || dest_y + copy_h > dest_h) {
        return ddraw::E_INVALIDARG;
    }

    // Scratch RECTs in DirectDraw object storage — avoid guest stack writes.
    if (!ensure_storage()) return 0x8007000Eu;
    const GuestAddr dest_rect = m_region_base + 0xF00;
    const GuestAddr src_rect_copy = m_region_base + 0xF10;
    (void)m_memory->write_u32(dest_rect + 0, dest_x);
    (void)m_memory->write_u32(dest_rect + 4, dest_y);
    (void)m_memory->write_u32(dest_rect + 8, dest_x + copy_w);
    (void)m_memory->write_u32(dest_rect + 12, dest_y + copy_h);
    (void)m_memory->write_u32(src_rect_copy + 0, static_cast<u32>(sl));
    (void)m_memory->write_u32(src_rect_copy + 4, static_cast<u32>(st));
    (void)m_memory->write_u32(src_rect_copy + 8, static_cast<u32>(sr));
    (void)m_memory->write_u32(src_rect_copy + 12, static_cast<u32>(sb));

    u32 blt_flags = 0x01000000;  // DDBLT_WAIT when DDBLTFAST_WAIT or default
    if ((flags & DDBLTFAST_WAIT) == 0) {
        // BltFast still completes synchronously in software; WAIT only affects
        // WASSTILLDRAWING, which we never return.
    }
    if ((flags & DDBLTFAST_SRCCOLORKEY) != 0) {
        blt_flags |= 0x00008000;  // DDBLT_KEYSRC
    }
    return surface_blt(dest, dest_rect, src, src_rect_copy, blt_flags, 0);
}

} // namespace vitreo::winapi
