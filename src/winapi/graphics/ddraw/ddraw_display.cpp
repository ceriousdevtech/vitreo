// SPDX-License-Identifier: LGPL-2.1-only

/// @file ddraw_display.cpp
/// @brief IDirectDraw cooperative level, display mode, mode enumeration, caps and
///        vertical-blank timing.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "graphics/contracts/presentation_state.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"

#include <chrono>
#include <thread>

namespace vitreo::winapi {

namespace {

// DirectDraw exposes raster timing even when drawing is emulated in software.
// Vitreo does not own the macOS display mode, so model the default 60 Hz scan
// with a monotonic clock. The model is shared by status queries and blocking
// waits, which preserves their observable ordering without tying Win32 state
// to Metal's asynchronous drawable queue.
using VBlankClock = std::chrono::steady_clock;
constexpr auto VBLANK_PERIOD = std::chrono::nanoseconds(16'666'667);
constexpr auto VBLANK_DURATION = std::chrono::nanoseconds(1'500'000);
constexpr auto VBLANK_BEGIN = VBLANK_PERIOD - VBLANK_DURATION;

std::chrono::nanoseconds vblank_phase(VBlankClock::time_point now) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch());
    return elapsed % VBLANK_PERIOD;
}

bool is_vertical_blank(VBlankClock::time_point now) noexcept {
    return vblank_phase(now) >= VBLANK_BEGIN;
}

VBlankClock::time_point next_vblank_begin(VBlankClock::time_point now) noexcept {
    const auto phase = vblank_phase(now);
    if (phase >= VBLANK_BEGIN) return now;
    return now + (VBLANK_BEGIN - phase);
}

VBlankClock::time_point next_vblank_end(VBlankClock::time_point now) noexcept {
    const auto phase = vblank_phase(now);
    if (phase >= VBLANK_BEGIN) return now + (VBLANK_PERIOD - phase);
    return now + (VBLANK_PERIOD - phase) + VBLANK_DURATION;
}

} // namespace

u32 DirectDrawManager::set_cooperative_level(GuestAddr object, u32 hwnd, u32 flags) {
    Object* state = find_object(object);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    const bool normal = (flags & ddraw::DDSCL_NORMAL) != 0;
    const bool exclusive = (flags & ddraw::DDSCL_EXCLUSIVE) != 0;
    if (normal == exclusive ||
        ((flags & ddraw::DDSCL_FULLSCREEN) != 0 && !exclusive)) {
        return ddraw::E_INVALIDARG;
    }
    // A NULL window is legal with DDSCL_NORMAL on Windows (only exclusive
    // full-screen needs one), and callers probe DirectDraw availability with
    // exactly SetCooperativeLevel(NULL, DDSCL_NORMAL) followed by
    // CreateSurface(PRIMARY).
    //
    // Vitreo deliberately fails that probe today. Accepting it makes SDL 1.2
    // pick its DirectX video driver, whose init then completes without ever
    // establishing a screen depth — SDL reports "Invalid bits per pixel" and,
    // because the probe already succeeded, the app has no windib fallback left
    // and exits. Until the DX5 presentation path is finished (see below),
    // reporting DirectDraw as unusable is the accurate answer and keeps the
    // caller on a backend that works.
    //
    // To lift this, change the condition to `hwnd == 0 && exclusive` (the real
    // Windows rule) and finish the DX5 chain: the interfaces it walks are all
    // implemented up to this point (IDirectDraw2, EnumDisplayModes, GetCaps,
    // GetAvailableVidMem, surface QueryInterface), but SDL never asks Vitreo
    // for a bit depth, so whatever source it uses still has to be found.
    if (hwnd == 0) {
        VITREO_TRACE("[ddraw] SetCooperativeLevel(NULL) refused: the DirectDraw"
                     " presentation path is not finished yet\n");
        return ddraw::E_INVALIDARG;
    }
    state->hwnd = hwnd;
    state->cooperative_flags = flags;
    if (hwnd != 0) m_presentation.set_target(hwnd);
    VITREO_TRACE("[ddraw] SetCooperativeLevel object=0x%llx hwnd=0x%x flags=0x%x\n",
                 static_cast<unsigned long long>(object), hwnd, flags);

    // Drive host full-screen to match the guest's exclusive full-screen mode.
    // Windows would switch the display resolution; on macOS we instead size the
    // target window to cover the display and let the presenter scale the guest
    // back-buffer into it. Track the driven hwnd so a later switch back to
    // DDSCL_NORMAL (or a different exclusive target) restores the host window
    // and clears the logical screen metrics published by SetDisplayMode.
    const bool want_fullscreen =
        exclusive && (flags & ddraw::DDSCL_FULLSCREEN) != 0;
    if (want_fullscreen) {
        if (m_fullscreen_hwnd != 0 && m_fullscreen_hwnd != hwnd) {
            if (m_fullscreen_cb) m_fullscreen_cb(m_fullscreen_hwnd, false);
            if (m_display_mode_cb) {
                m_display_mode_cb(m_fullscreen_hwnd, 0, 0, 0);
            }
        }
        if (m_fullscreen_cb) m_fullscreen_cb(hwnd, true);
        m_fullscreen_hwnd = hwnd;
    } else if (m_fullscreen_hwnd != 0) {
        if (m_fullscreen_cb) m_fullscreen_cb(m_fullscreen_hwnd, false);
        if (m_display_mode_cb) {
            m_display_mode_cb(m_fullscreen_hwnd, 0, 0, 0);
        }
        m_fullscreen_hwnd = 0;
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::set_display_mode(GuestAddr object, u32 width, u32 height,
                                         u32 bits_per_pixel) {
    Object* state = find_object(object);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (width == 0 || height == 0 ||
        (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 24 &&
         bits_per_pixel != 32)) {
        return ddraw::E_INVALIDARG;
    }
    if ((state->cooperative_flags & ddraw::DDSCL_EXCLUSIVE) == 0) {
        return 0x887600E1u; // DDERR_NOEXCLUSIVEMODE
    }
    m_presentation.set_display_mode(
        graphics::DisplayMode{width, height, bits_per_pixel, 0});
    // Publish the logical mode to user32/gdi. Without this, GetSystemMetrics
    // and GetDeviceCaps keep reporting the host display size while mouse
    // coordinates are mapped into the 640x480 (etc.) surface. Edge-scroll
    // zones then miss on the left and bottom of the letterboxed view.
    if (m_display_mode_cb) {
        m_display_mode_cb(state->hwnd, width, height, bits_per_pixel);
    }
    VITREO_TRACE("[ddraw] SetDisplayMode object=0x%llx %ux%ux%u\n",
                 static_cast<unsigned long long>(object), width, height,
                 bits_per_pixel);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::enum_display_modes(GuestAddr object, u32 flags,
                                          GuestAddr descriptor,
                                          GuestAddr context,
                                          GuestAddr callback) {
    if (!m_memory || !find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (callback == 0) return ddraw::E_INVALIDARG;
    if (!m_guest_call) return ddraw::E_NOTIMPL;
    (void)flags;

    // A caller may restrict the enumeration by passing a descriptor; Vitreo
    // reports the modes it can actually present, so an unfiltered walk is the
    // honest answer and any filtering the caller wanted it applies itself.
    (void)descriptor;

    u32 host_width = 0;
    u32 host_height = 0;
    if (m_display_size_cb) m_display_size_cb(host_width, host_height);
    if (host_width == 0 || host_height == 0) return ddraw::E_INVALIDARG;

    // Every mode is presented by scaling into the target window, so the list
    // is the standard set bounded by the host display rather than anything
    // the hardware enumerates.
    struct Mode { u32 width; u32 height; };
    static constexpr Mode kModes[] = {
        {320, 200},  {320, 240},  {400, 300},  {512, 384},
        {640, 400},  {640, 480},  {800, 500},  {800, 600},
        {1024, 640}, {1024, 768}, {1152, 864}, {1280, 720},
        {1280, 800}, {1280, 960}, {1280, 1024}, {1440, 900},
        {1600, 900}, {1600, 1000}, {1600, 1200}, {1680, 1050},
        {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600},
        {3840, 2160},
    };
    static constexpr u32 kBitDepths[] = {8, 16, 32};

    // DDSURFACEDESC is 108 bytes. The enumeration callback receives a pointer
    // to one plus the caller's context, and returns DDENUMRET_CANCEL (0) to
    // stop.
    constexpr u32 DDSD_HEIGHT = 0x00000002;
    constexpr u32 DDSD_WIDTH  = 0x00000004;
    constexpr u32 DDSD_PITCH  = 0x00000008;
    constexpr u32 DDSD_PIXELFORMAT = 0x00001000;
    constexpr u32 DDPF_RGB = 0x00000040;
    constexpr u32 DDPF_PALETTEINDEXED8 = 0x00000020;
    constexpr u32 DDENUMRET_CANCEL = 0;
    constexpr usize DESC_SIZE = 108;

    const GuestAddr scratch =
        m_memory->find_free_gap(0x62800000, 0x1000);
    if (scratch == 0 ||
        m_memory->map_region(scratch, 0x1000,
                             MemoryProtection::ReadWrite).is_err()) {
        return 0x8007000Eu;
    }

    u32 enumerated = 0;
    for (const auto& mode : kModes) {
        if (mode.width > host_width || mode.height > host_height) continue;
        for (const u32 bpp : kBitDepths) {
            for (usize offset = 0; offset < DESC_SIZE; offset += 4) {
                (void)m_memory->write_u32(scratch + offset, 0);
            }
            (void)m_memory->write_u32(scratch + 0, DESC_SIZE);       // dwSize
            (void)m_memory->write_u32(scratch + 4,
                                      DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH |
                                      DDSD_PIXELFORMAT);            // dwFlags
            (void)m_memory->write_u32(scratch + 8, mode.height);
            (void)m_memory->write_u32(scratch + 12, mode.width);
            (void)m_memory->write_u32(scratch + 16,
                                      (mode.width * bpp + 7) / 8);  // lPitch
            // DDPIXELFORMAT at +72: dwSize, dwFlags, dwFourCC, dwRGBBitCount,
            // then the R/G/B/A masks.
            (void)m_memory->write_u32(scratch + 72, 32);
            (void)m_memory->write_u32(scratch + 76,
                                      bpp == 8 ? DDPF_PALETTEINDEXED8
                                               : DDPF_RGB);
            (void)m_memory->write_u32(scratch + 84, bpp);
            if (bpp == 16) {
                (void)m_memory->write_u32(scratch + 88, 0xF800);
                (void)m_memory->write_u32(scratch + 92, 0x07E0);
                (void)m_memory->write_u32(scratch + 96, 0x001F);
            } else if (bpp == 32) {
                (void)m_memory->write_u32(scratch + 88, 0x00FF0000);
                (void)m_memory->write_u32(scratch + 92, 0x0000FF00);
                (void)m_memory->write_u32(scratch + 96, 0x000000FF);
            }

            const u64 args[2] = {static_cast<u64>(scratch),
                                 static_cast<u64>(context)};
            const u64 result = m_guest_call(callback, args, 2, false);
            ++enumerated;
            if (static_cast<u32>(result) == DDENUMRET_CANCEL) {
                VITREO_TRACE("[ddraw] EnumDisplayModes cancelled after %u\n",
                             enumerated);
                (void)m_memory->unmap_region(scratch, 0x1000);
                return ddraw::DD_OK;
            }
        }
    }
    VITREO_TRACE("[ddraw] EnumDisplayModes enumerated %u modes\n", enumerated);
    (void)m_memory->unmap_region(scratch, 0x1000);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::get_display_mode(GuestAddr object,
                                        GuestAddr descriptor) {
    if (!m_memory || !find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (descriptor == 0) return ddraw::E_INVALIDARG;
    if (m_memory->read_u32(descriptor).value_or(0) != 108) {
        return ddraw::E_INVALIDARG;
    }

    // Report the mode the guest set, or the host display when it has not set
    // one — the same rule CreateSurface uses for a primary surface. Callers
    // read the bit depth from here to decide their pixel format, so an empty
    // answer reads as "0 bits per pixel" and fails their video init.
    graphics::DisplayMode mode = m_presentation.display_mode();
    if (mode.width == 0 || mode.height == 0) {
        u32 host_width = 0;
        u32 host_height = 0;
        if (m_display_size_cb) m_display_size_cb(host_width, host_height);
        if (host_width == 0 || host_height == 0) return ddraw::E_INVALIDARG;
        mode.width = host_width;
        mode.height = host_height;
    }
    if (mode.bits_per_pixel == 0) mode.bits_per_pixel = 32;

    constexpr u32 DDSD_HEIGHT = 0x00000002;
    constexpr u32 DDSD_WIDTH  = 0x00000004;
    constexpr u32 DDSD_PITCH  = 0x00000008;
    constexpr u32 DDSD_PIXELFORMAT = 0x00001000;
    constexpr u32 DDPF_RGB = 0x00000040;
    constexpr u32 DDPF_PALETTEINDEXED8 = 0x00000020;

    for (usize offset = 4; offset < 108; offset += 4) {
        (void)m_memory->write_u32(descriptor + offset, 0);
    }
    (void)m_memory->write_u32(descriptor + 4,
                              DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH |
                              DDSD_PIXELFORMAT);
    (void)m_memory->write_u32(descriptor + 8, mode.height);
    (void)m_memory->write_u32(descriptor + 12, mode.width);
    (void)m_memory->write_u32(descriptor + 16,
                              (mode.width * mode.bits_per_pixel + 7) / 8);
    (void)m_memory->write_u32(descriptor + 72, 32);  // DDPIXELFORMAT dwSize
    (void)m_memory->write_u32(descriptor + 76,
                              mode.bits_per_pixel == 8 ? DDPF_PALETTEINDEXED8
                                                       : DDPF_RGB);
    (void)m_memory->write_u32(descriptor + 84, mode.bits_per_pixel);
    if (mode.bits_per_pixel == 16) {
        (void)m_memory->write_u32(descriptor + 88, 0xF800);
        (void)m_memory->write_u32(descriptor + 92, 0x07E0);
        (void)m_memory->write_u32(descriptor + 96, 0x001F);
    } else if (mode.bits_per_pixel == 32 || mode.bits_per_pixel == 24) {
        (void)m_memory->write_u32(descriptor + 88, 0x00FF0000);
        (void)m_memory->write_u32(descriptor + 92, 0x0000FF00);
        (void)m_memory->write_u32(descriptor + 96, 0x000000FF);
    }
    VITREO_TRACE("[ddraw] GetDisplayMode -> %ux%ux%u\n", mode.width,
                 mode.height, mode.bits_per_pixel);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::get_caps(GuestAddr object, GuestAddr driver_caps,
                                GuestAddr hel_caps) {
    if (!m_memory || !find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (driver_caps == 0 && hel_caps == 0) return ddraw::E_INVALIDARG;

    // IDirectDraw::GetCaps reports the hardware driver and the Hardware
    // Emulation Layer independently. Vitreo has no hardware DirectDraw
    // blitter, but its synchronous software HEL implements Blt/BltFast,
    // color-fill, system-memory sources, and source color keys.
    constexpr u32 DDCAPS_BLT = 0x00000040;
    constexpr u32 DDCAPS_GDI = 0x00000400;
    constexpr u32 DDCAPS_PALETTE = 0x00008000;
    constexpr u32 DDCAPS_COLORKEY = 0x00400000;
    constexpr u32 DDCAPS_NOHARDWARE = 0x02000000;
    constexpr u32 DDCAPS_BLTCOLORFILL = 0x04000000;
    constexpr u32 DDCAPS_CANBLTSYSMEM = 0x80000000;
    constexpr u32 DDCKEYCAPS_SRCBLT = 0x00000200;
    constexpr u32 POOL_BYTES = 64u * 1024u * 1024u;

    // DDCAPS member offsets. Every documented version of the structure
    // (DX1/DX3/DX5/DX7) shares this prefix, so the offsets hold whatever
    // dwSize the caller passed.
    constexpr u32 OFF_CAPS = 4;
    constexpr u32 OFF_CKEY_CAPS = 12;
    constexpr u32 OFF_VIDMEM_TOTAL = 60;
    constexpr u32 OFF_VIDMEM_FREE = 64;

    const auto fill = [&](GuestAddr caps, u32 capability_flags,
                          u32 color_key_flags) {
        if (caps == 0) return;
        auto size = m_memory->read_u32(caps);
        const u32 struct_size = size.is_ok() && size.value() >= 8
                                    ? size.value() : 0;
        if (struct_size == 0) return;
        for (u32 offset = 4; offset < struct_size; offset += 4) {
            (void)m_memory->write_u32(caps + offset, 0);
        }
        (void)m_memory->write_u32(caps + OFF_CAPS, capability_flags);
        if (struct_size >= OFF_CKEY_CAPS + 4) {
            (void)m_memory->write_u32(caps + OFF_CKEY_CAPS, color_key_flags);
        }
        // dwVidMemTotal is documented to agree with GetAvailableVidMem, which
        // reports the same pool. Leaving it zero reads as "no video memory",
        // which pushes callers into keeping every surface in system memory.
        if (struct_size >= OFF_VIDMEM_FREE + 4) {
            (void)m_memory->write_u32(caps + OFF_VIDMEM_TOTAL, POOL_BYTES);
            (void)m_memory->write_u32(caps + OFF_VIDMEM_FREE, POOL_BYTES);
        }
    };
    // DDCAPS_3D advertises a hardware 3D rasteriser, and DDCAPS_NOHARDWARE is
    // its opposite: "no hardware support". Both must follow whether a backend
    // device is actually present, because a caller that believes the former
    // will go on to create a Direct3D device and draw through it. The
    // Metal-backed D3D7 path satisfies that claim -- DrawPrimitive reaches
    // guest surface memory (D3D7DrawTest.DrawPrimitiveReachesSurfaceMemory) --
    // so the flag is a report of a capability that exists, not an assertion
    // made to get a caller past its check. With no backend, the honest answer
    // is still NOHARDWARE.
    const bool has_3d = graphics::active_backend_device() != nullptr;
    constexpr u32 DDCAPS_3D = 0x00000001;
    const u32 driver_flags =
        DDCAPS_GDI | DDCAPS_PALETTE |
        (has_3d ? DDCAPS_3D : DDCAPS_NOHARDWARE);
    const u32 hel_flags = DDCAPS_BLT | DDCAPS_GDI | DDCAPS_PALETTE |
                          DDCAPS_COLORKEY | DDCAPS_BLTCOLORFILL |
                          DDCAPS_CANBLTSYSMEM;
    fill(driver_caps, driver_flags, 0);
    fill(hel_caps, hel_flags, DDCKEYCAPS_SRCBLT);
    // dwSize identifies which DDCAPS revision the caller expects, which is
    // what decides how many fields it will read back.
    u32 driver_size = 0;
    if (driver_caps != 0) {
        if (auto r = m_memory->read_u32(driver_caps); r.is_ok()) {
            driver_size = r.value();
        }
    }
    VITREO_TRACE("[ddraw] GetCaps dwSize=%u (DDCAPS_DX%s)\n", driver_size,
                 driver_size >= 380 ? "7" : (driver_size >= 316 ? "5" : "1/3"));
    VITREO_TRACE("[ddraw] GetCaps driver=0x%llx caps=0x%x "
                 "hel=0x%llx caps=0x%x ckey=0x%x\n",
                 static_cast<unsigned long long>(driver_caps),
                 driver_flags, static_cast<unsigned long long>(hel_caps),
                 hel_flags, DDCKEYCAPS_SRCBLT);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::get_vertical_blank_status(GuestAddr object,
                                                  GuestAddr status) {
    // Microsoft IDirectDraw::GetVerticalBlankStatus: write a BOOL indicating
    // whether vertical blank is occurring; invalid output pointers fail with
    // DDERR_INVALIDPARAMS.
    if (!m_memory || !find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (status == 0 ||
        m_memory->write_u32(status, is_vertical_blank(VBlankClock::now()) ? 1u
                                                                         : 0u)
            .is_err()) {
        return ddraw::DDERR_INVALIDPARAMS;
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::wait_for_vertical_blank(GuestAddr object, u32 flags,
                                                GuestAddr event) {
    // Microsoft IDirectDraw::WaitForVerticalBlank: BLOCKBEGIN and BLOCKEND
    // synchronously wait for the corresponding raster boundary. The event
    // flag is explicitly unsupported by DirectDraw; hEvent is otherwise
    // unused by the documented contract.
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    constexpr u32 DDWAITVB_BLOCKBEGIN = 0x00000001;
    constexpr u32 DDWAITVB_BLOCKBEGINEVENT = 0x00000002;
    constexpr u32 DDWAITVB_BLOCKEND = 0x00000004;
    if (flags == DDWAITVB_BLOCKBEGINEVENT) return ddraw::E_NOTIMPL;
    if (flags != DDWAITVB_BLOCKBEGIN && flags != DDWAITVB_BLOCKEND) {
        return ddraw::DDERR_INVALIDPARAMS;
    }
    (void)event;

    const auto started = VBlankClock::now();
    const auto boundary = flags == DDWAITVB_BLOCKBEGIN
                              ? next_vblank_begin(started)
                              : next_vblank_end(started);
    if (boundary > started) std::this_thread::sleep_until(boundary);
    const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
        VBlankClock::now() - started);
    VITREO_TRACE("[ddraw] WaitForVerticalBlank flags=0x%x event=0x%llx "
                 "waited_us=%lld\n",
                 flags, static_cast<unsigned long long>(event),
                 static_cast<long long>(waited.count()));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::restore_display_mode(GuestAddr object) {
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    // The mode to restore is the host desktop's. Vitreo never actually changes
    // the display, so this only has to put the presentation state back where
    // an un-moded guest would find it.
    u32 host_width = 0;
    u32 host_height = 0;
    if (m_display_size_cb) m_display_size_cb(host_width, host_height);
    if (host_width != 0 && host_height != 0) {
        graphics::DisplayMode mode = m_presentation.display_mode();
        mode.width = host_width;
        mode.height = host_height;
        if (mode.bits_per_pixel == 0) mode.bits_per_pixel = 32;
        m_presentation.set_display_mode(mode);
    }
    VITREO_TRACE("[ddraw] RestoreDisplayMode object=0x%llx -> %ux%u\n",
                 static_cast<unsigned long long>(object), host_width,
                 host_height);
    return ddraw::DD_OK;
}

} // namespace vitreo::winapi
