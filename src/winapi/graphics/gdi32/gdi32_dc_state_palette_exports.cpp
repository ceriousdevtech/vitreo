// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_dc_state_palette_exports.cpp
/// @brief DC-state, clipping and palette exports.
///
/// GetDCOrgEx .. StretchDIBits: SaveDC/RestoreDC, Ellipse, clip
/// rectangles and regions, the Pinball palette block and the DIB
/// transfer calls.
///
/// One of the section registrars split out of make_gdi32_exports
/// (gdi32_registration.cpp); see gdi32_registration_internal.hpp. The entries
/// are pushed in their original order.

#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/kernel32/error.hpp"

namespace vitreo::winapi {

void add_gdi32_dc_state_palette_exports(DllExports& exports, GdiManager& mgr) {
    // ---- GetDCOrgEx(hdc, lppt) ----
    exports.entries.push_back({"GetDCOrgEx", 46, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto lppt = m.arg_ptr(1);
        auto ok = mgr.get_dc_org_ex(hdc, lppt);
        m.set_return(ok ? 1u : 0u);
    }, 8});

    // ---- SaveDC(hdc) ---- returns save level or 0 on failure
    exports.entries.push_back({"SaveDC", 0, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto level = mgr.save_dc(hdc);
        m.set_return(static_cast<u32>(level));
    }, 4});

    // ---- RestoreDC(hdc, nSavedDC) ----
    exports.entries.push_back({"RestoreDC", 0, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto nSavedDC = static_cast<i32>(m.arg(1));
        auto ok = mgr.restore_dc(hdc, nSavedDC);
        m.set_return(ok ? 1u : 0u);
    }, 8});

    // ---- Ellipse(hdc, left, top, right, bottom) ----
    exports.entries.push_back({"Ellipse", 0, [&mgr](Marshaler& m) {
        auto hdc    = static_cast<Handle>(m.arg(0));
        auto left   = static_cast<i32>(m.arg(1));
        auto top    = static_cast<i32>(m.arg(2));
        auto right  = static_cast<i32>(m.arg(3));
        auto bottom = static_cast<i32>(m.arg(4));
        auto ok = mgr.ellipse(hdc, left, top, right, bottom);
        m.set_return(ok ? 1u : 0u);
    }, 20});

    // ---- GdiFlush() ----
    exports.entries.push_back({"GdiFlush", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.flush_pending_pixels() ? 1u : 0u);
    }, 0});

    // ---- ExcludeClipRect(hdc, left, top, right, bottom) ----
    exports.entries.push_back({"ExcludeClipRect", 0, [&mgr](Marshaler& m) {
        auto hdc    = static_cast<Handle>(m.arg(0));
        auto left   = static_cast<i32>(m.arg(1));
        auto top    = static_cast<i32>(m.arg(2));
        auto right  = static_cast<i32>(m.arg(3));
        auto bottom = static_cast<i32>(m.arg(4));
        auto result = mgr.exclude_clip_rect(hdc, left, top, right, bottom);
        m.set_return(static_cast<u32>(result));
    }, 20});

    // ---- IntersectClipRect(hdc, left, top, right, bottom) ----
    exports.entries.push_back({"IntersectClipRect", 0, [&mgr](Marshaler& m) {
        auto hdc    = static_cast<Handle>(m.arg(0));
        auto left   = static_cast<i32>(m.arg(1));
        auto top    = static_cast<i32>(m.arg(2));
        auto right  = static_cast<i32>(m.arg(3));
        auto bottom = static_cast<i32>(m.arg(4));
        auto result = mgr.intersect_clip_rect(hdc, left, top, right, bottom);
        m.set_return(static_cast<u32>(result));
    }, 20});

    // ---- GetClipBox(hdc, lpRect) ----
    exports.entries.push_back({"GetClipBox", 0, [&mgr](Marshaler& m) {
        auto hdc    = static_cast<Handle>(m.arg(0));
        auto lpRect = m.arg_ptr(1);
        i32 left = 0, top = 0, right = 0, bottom = 0;
        auto result = mgr.get_clip_box(hdc, left, top, right, bottom);
        if (lpRect && result != RGN_ERROR) {
            // RECT struct: { LONG left; LONG top; LONG right; LONG bottom; }
            (void)m.write_u32(lpRect,      static_cast<u32>(left));
            (void)m.write_u32(lpRect + 4,  static_cast<u32>(top));
            (void)m.write_u32(lpRect + 8,  static_cast<u32>(right));
            (void)m.write_u32(lpRect + 12, static_cast<u32>(bottom));
        }
        m.set_return(static_cast<u32>(result));
    }, 8});

    // ---- SelectClipRgn(hdc, hrgn) ----
    exports.entries.push_back({"SelectClipRgn", 0, [&mgr](Marshaler& m) {
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto hrgn = static_cast<Handle>(m.arg(1));
        auto result = mgr.select_clip_rgn(hdc, hrgn);
        m.set_return(static_cast<u32>(result));
    }, 8});

    // ---- CreateRectRgn(left, top, right, bottom) ----
    exports.entries.push_back({"CreateRectRgn", 0, [&mgr](Marshaler& m) {
        auto left   = static_cast<i32>(m.arg(0));
        auto top    = static_cast<i32>(m.arg(1));
        auto right  = static_cast<i32>(m.arg(2));
        auto bottom = static_cast<i32>(m.arg(3));
        auto h = mgr.create_rect_rgn(left, top, right, bottom);
        m.set_return(static_cast<u64>(h));
    }, 16});

    // ---- CreateRectRgnIndirect(lpRect) ----
    exports.entries.push_back({"CreateRectRgnIndirect", 0, [&mgr](Marshaler& m) {
        auto lpRect = m.arg_ptr(0);
        i32 left = 0, top = 0, right = 0, bottom = 0;
        if (lpRect) {
            auto r_left   = m.read_u32(lpRect);
            auto r_top    = m.read_u32(lpRect + 4);
            auto r_right  = m.read_u32(lpRect + 8);
            auto r_bottom = m.read_u32(lpRect + 12);
            if (r_left.is_ok())   left   = static_cast<i32>(r_left.value());
            if (r_top.is_ok())    top    = static_cast<i32>(r_top.value());
            if (r_right.is_ok())  right  = static_cast<i32>(r_right.value());
            if (r_bottom.is_ok()) bottom = static_cast<i32>(r_bottom.value());
        }
        auto h = mgr.create_rect_rgn(left, top, right, bottom);
        m.set_return(static_cast<u64>(h));
    }, 4});

    // ---- GetBkMode(hdc) ----
    exports.entries.push_back({"GetBkMode", 0, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto mode = mgr.get_bk_mode(hdc);
        m.set_return(static_cast<u32>(mode));
    }, 4});

    // ---- GetTextColor(hdc) ----
    exports.entries.push_back({"GetTextColor", 0, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto color = mgr.get_text_color(hdc);
        m.set_return(color);
    }, 4});

    // --- Pinball.exe palette & DIB stubs ---

    // CreatePalette(lpLogPalette) → HPALETTE
    exports.entries.push_back({"CreatePalette", 0, [&mgr](Marshaler& m) {
        auto lpLogPalette = m.arg_ptr(0);
        auto h = mgr.create_palette(lpLogPalette);
        m.set_return(static_cast<u64>(h));
    }, 4});

    // CreateHalftonePalette(hdc) → HPALETTE
    exports.entries.push_back({"CreateHalftonePalette", 0, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto h = mgr.create_halftone_palette(hdc);
        m.set_return(static_cast<u64>(h));
    }, 4});

    // GetDIBColorTable(hdc, iStart, cEntries, prgbq) → UINT
    exports.entries.push_back({"GetDIBColorTable", 0, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto iStart = static_cast<u32>(m.arg(1));
        auto cEntries = static_cast<u32>(m.arg(2));
        auto prgbq = m.arg_ptr(3);
        auto count = mgr.get_dib_color_table(hdc, iStart, cEntries, prgbq);
        m.set_return(static_cast<u64>(count));
    }, 16});

    // SelectPalette(hdc, hPal, bForceBackground) → HPALETTE
    exports.entries.push_back({"SelectPalette", 0, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto hPal = static_cast<Handle>(m.arg(1));
        auto bForceBackground = m.arg(2) != 0;
        auto prev = mgr.select_palette(hdc, hPal, bForceBackground);
        m.set_return(static_cast<u64>(prev));
    }, 12});

    // RealizePalette(hdc) → UINT
    exports.entries.push_back({"RealizePalette", 0, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto mapped = mgr.realize_palette(hdc);
        m.set_return(static_cast<u64>(mapped));
    }, 4});

    // GetPaletteEntries(hPal, iStart, cEntries, pPE) → UINT
    exports.entries.push_back({"GetPaletteEntries", 0, [&mgr](Marshaler& m) {
        auto hPal = static_cast<Handle>(m.arg(0));
        auto iStart = static_cast<u32>(m.arg(1));
        auto cEntries = static_cast<u32>(m.arg(2));
        auto pPE = m.arg_ptr(3);
        auto count = mgr.get_palette_entries(hPal, iStart, cEntries, pPE);
        m.set_return(static_cast<u64>(count));
    }, 16});

    // SetPaletteEntries(hPal, iStart, cEntries, pPE) → UINT
    exports.entries.push_back({"SetPaletteEntries", 0, [&mgr](Marshaler& m) {
        auto hPal = static_cast<Handle>(m.arg(0));
        auto iStart = static_cast<u32>(m.arg(1));
        auto cEntries = static_cast<u32>(m.arg(2));
        auto pPE = m.arg_ptr(3);
        auto count = mgr.set_palette_entries(hPal, iStart, cEntries, pPE);
        m.set_return(static_cast<u64>(count));
    }, 16});

    // ResizePalette(hPal, n) → BOOL
    exports.entries.push_back({"ResizePalette", 0, [&mgr](Marshaler& m) {
        auto hPal = static_cast<Handle>(m.arg(0));
        auto cEntries = static_cast<u32>(m.arg(1));
        auto ok = mgr.resize_palette(hPal, cEntries);
        m.set_return(ok ? 1u : 0u);
    }, 8});

    // GetSystemPaletteEntries(hdc, iStart, cEntries, pPE) → UINT
    exports.entries.push_back({"GetSystemPaletteEntries", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto hdc = static_cast<Handle>(m.arg(0));
        auto iStart = static_cast<u32>(m.arg(1));
        auto cEntries = static_cast<u32>(m.arg(2));
        auto pPE = m.arg_ptr(3);
        auto count = mgr.get_system_palette_entries(hdc, iStart, cEntries, pPE);
        m.set_return(static_cast<u64>(count));
    }, 16});

    // SetSystemPaletteUse(hdc, uUsage) → UINT
    exports.entries.push_back({"SetSystemPaletteUse", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        auto hdc = static_cast<Handle>(m.arg(0));
        auto uUsage = static_cast<u32>(m.arg(1));
        auto prev = mgr.set_system_palette_use(hdc, uUsage);
        m.set_return(static_cast<u64>(prev));
    }, 8});

    // GetSystemPaletteUse(hdc) -> UINT
    exports.entries.push_back({"GetSystemPaletteUse", 0, [&mgr](Marshaler& m) {
        const auto hdc = static_cast<Handle>(m.arg(0));
        m.set_return(static_cast<u64>(mgr.get_system_palette_use(hdc)));
    }, 4});

    // CreateDIBitmap(hdc, lpbmih, fdwInit, lpbInit, lpbmi, fuUsage) → HBITMAP
    //
    // MS Learn: with CBM_INIT this is CreateCompatibleBitmap + SetDIBits.
    // Callers load a DIB through CreateDIBitmap(CBM_INIT=4). Ignoring
    // lpbInit leaves the HBITMAP empty, and BitBlt/flush presents a blank
    // window.
    exports.entries.push_back({"CreateDIBitmap", 0, [&mgr](Marshaler& m) {
        auto hdc     = static_cast<Handle>(m.arg(0));
        auto lpbmih  = m.arg_ptr(1);
        auto fdwInit = static_cast<u32>(m.arg(2));
        auto lpbInit = static_cast<GuestAddr>(m.arg(3));
        auto lpbmi   = static_cast<GuestAddr>(m.arg(4));
        auto fuUsage = static_cast<u32>(m.arg(5));
        if (hdc == 0 && lpbmih == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        constexpr u32 CBM_INIT = 0x04;

        // Read width and height from BITMAPINFOHEADER (biWidth at +4, biHeight at +8)
        i32 width = 1, height = 1;
        if (lpbmih) {
            auto w_r = m.read_u32(lpbmih + 4);
            auto h_r = m.read_u32(lpbmih + 8);
            if (w_r.is_ok()) width = static_cast<i32>(w_r.value());
            if (h_r.is_ok()) height = static_cast<i32>(h_r.value());
            if (height < 0) height = -height; // top-down DIB
            if (width < 0) width = -width;
        }
        if (width <= 0) width = 1;
        if (height <= 0) height = 1;

        auto h = mgr.create_compatible_bitmap(hdc, width, height);
        if (h != 0 && (fdwInit & CBM_INIT) != 0 && lpbInit != 0 && lpbmi != 0) {
            // Prefer full BITMAPINFO at lpbmi (color table + bpp); fall back to
            // the info header alone when callers pass the same pointer twice.
            const GuestAddr bmi = lpbmi != 0 ? lpbmi : lpbmih;
            (void)mgr.set_dib_bits(hdc, h, 0, static_cast<u32>(height),
                                   lpbInit, bmi, fuUsage);
        }
        VITREO_TRACE("[gdi] CreateDIBitmap %dx%d init=0x%x bits=0x%llx "
                     "bmi=0x%llx -> 0x%x\n",
                     width, height, fdwInit,
                     static_cast<unsigned long long>(lpbInit),
                     static_cast<unsigned long long>(lpbmi), h);
        m.set_return(static_cast<u64>(h));
    }, 24});

    // StretchDIBits(hdc,xD,yD,wD,hD,xS,yS,wS,hS,lpBits,lpBMI,iUsage,dwRop) → int
    exports.entries.push_back({"StretchDIBits", 0, [&mgr](Marshaler& m) {
        auto hdcDest = static_cast<Handle>(m.arg(0));
        auto xDest   = static_cast<i32>(m.arg(1));
        auto yDest   = static_cast<i32>(m.arg(2));
        auto wDest   = static_cast<i32>(m.arg(3));
        auto hDest   = static_cast<i32>(m.arg(4));
        auto xSrc    = static_cast<i32>(m.arg(5));
        auto ySrc    = static_cast<i32>(m.arg(6));
        auto wSrc    = static_cast<i32>(m.arg(7));
        auto hSrc    = static_cast<i32>(m.arg(8));
        auto lpBits  = m.arg_ptr(9);
        auto lpBMI   = m.arg_ptr(10);
        auto iUsage  = static_cast<u32>(m.arg(11));
        auto dwRop   = static_cast<u32>(m.arg(12));
        auto ret = mgr.stretch_dib_bits(m, hdcDest, xDest, yDest, wDest, hDest,
                                         xSrc, ySrc, wSrc, hSrc,
                                         lpBits, lpBMI, iUsage, dwRop);
        m.set_return(static_cast<u64>(static_cast<u32>(ret)));
    }, 52});
}

}  // namespace vitreo::winapi
