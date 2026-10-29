// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_dc_attribute_exports.cpp
/// @brief DC-attribute and bitmap-creation exports.
///
/// GetTextFaceW .. GetBkColor: text face and font data, map and graphics
/// modes, world transform, GetDIBits, bitmap creation and CreateDCA.
/// Reads GdiManager::m_dc_states and m_region_objects directly.
///
/// One of the section registrars split out of make_gdi32_exports
/// (gdi32_registration.cpp); see gdi32_registration_internal.hpp. The entries
/// are pushed in their original order.

#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/kernel32/error.hpp"

namespace vitreo::winapi {

void add_gdi32_dc_attribute_exports(DllExports& exports, GdiManager& mgr) {
    // ---- GetTextFaceW(hdc, nCount, lpFaceName) ----
    exports.entries.push_back({"GetTextFaceW", 0, [&mgr](Marshaler& m) { gdi_text_exports::get_text_face(mgr, m, true); }, 12});

    // ANSI sibling of GetTextFaceW. qwindows IAT-imports this; the fail-thunk
    // returned 0xFFFFFFFF as a character count.
    exports.entries.push_back({"GetTextFaceA", 0, [&mgr](Marshaler& m) { gdi_text_exports::get_text_face(mgr, m, false); }, 12});

    // DWORD GetFontUnicodeRanges(HDC, LPGLYPHSET)
    // Size query with lpgs=NULL must return the GLYPHSET byte count. The
    // fail-thunk returned 0xFFFFFFFF, which Qt treats as an allocation size.
    exports.entries.push_back({"GetFontUnicodeRanges", 0, [](Marshaler& m) {
        if (m.arg(0) == 0) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        constexpr u32 kHeader = 16;
        constexpr u32 kRange = 4;
        constexpr u32 kSize = kHeader + kRange;
        const GuestAddr gs = m.arg_ptr(1);
        if (gs == 0) {
            m.set_return(kSize);
            return;
        }
        const u32 capacity = m.read_u32(gs).value_or(0);
        if (capacity < kSize) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        (void)m.write_u32(gs + 0, kSize);
        (void)m.write_u32(gs + 4, 0);
        (void)m.write_u32(gs + 8, 0xE0u); // U+0020..U+00FF
        (void)m.write_u32(gs + 12, 1);
        (void)m.write_u16(gs + 16, 0x20);
        (void)m.write_u16(gs + 18, 0xE0);
        m.set_return(kSize);
    }, 8});

    // ---- GetFontData(hdc, table, offset, buffer, buffer_size) ----
    // Vitreo's current font backend does not retain the selected font's raw
    // TrueType/OpenType bytes. Fail honestly with GDI_ERROR until that data is
    // available, but register the real five-argument WINAPI signature. A
    // generic missing-export thunk has no stdcall cleanup metadata and leaves
    // 20 bytes of arguments on an x86 caller's stack.
    exports.entries.push_back({"GetFontData", 0, [&mgr](Marshaler& m) {
        const u32 result = gdi_font_data::get_font_data(mgr, m);
        VITREO_TRACE(
            "[gdi] GetFontData hdc=0x%x table=0x%08x offset=%u size=%u -> %u\n",
            static_cast<Handle>(m.arg(0)), static_cast<u32>(m.arg(1)),
            static_cast<u32>(m.arg(2)), static_cast<u32>(m.arg(4)), result);
        m.set_return(result);
    }, 20});

    // ---- SetPixelV(hdc, x, y, color) ----
    exports.entries.push_back({"SetPixelV", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto hdc   = static_cast<Handle>(m.arg(0));
        auto x     = static_cast<i32>(m.arg(1));
        auto y     = static_cast<i32>(m.arg(2));
        auto color = static_cast<u32>(m.arg(3));
        mgr.set_pixel(hdc, x, y, color);
        m.set_return(1);
    }, 16});

    // ---- RectVisible(hdc, lprc) ----
    exports.entries.push_back({"RectVisible", 0, [](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        (void)m.arg(0); (void)m.arg(1);
        m.set_return(1); // always visible
    }, 8});

    // ---- GetWindowExtEx(hdc, lpsize) ----
    exports.entries.push_back({"GetWindowExtEx", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        auto hdc    = static_cast<Handle>(m.arg(0));
        auto lpsize = m.arg_ptr(1);
        if (lpsize) {
            auto it = mgr.m_dc_states.find(hdc);
            i32 cx = 1, cy = 1;
            if (it != mgr.m_dc_states.end()) {
                cx = it->second.window_ext_x;
                cy = it->second.window_ext_y;
            }
            (void)m.write_u32(lpsize,     static_cast<u32>(cx));
            (void)m.write_u32(lpsize + 4, static_cast<u32>(cy));
        }
        m.set_return(1);
    }, 8});

    // ---- GetViewportExtEx(hdc, lpsize) ----
    exports.entries.push_back({"GetViewportExtEx", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        auto hdc    = static_cast<Handle>(m.arg(0));
        auto lpsize = m.arg_ptr(1);
        if (lpsize) {
            auto it = mgr.m_dc_states.find(hdc);
            i32 cx = 1, cy = 1;
            if (it != mgr.m_dc_states.end()) {
                cx = it->second.viewport_ext_x;
                cy = it->second.viewport_ext_y;
            }
            (void)m.write_u32(lpsize,     static_cast<u32>(cx));
            (void)m.write_u32(lpsize + 4, static_cast<u32>(cy));
        }
        m.set_return(1);
    }, 8});

    // ---- SetTextAlign(hdc, fMode) ----
    exports.entries.push_back({"SetTextAlign", 0, [&mgr](Marshaler& m) { gdi_text_exports::set_text_align(mgr, m); }, 8});

    // ---- SetMapMode(hdc, fnMapMode) ----
    exports.entries.push_back({"SetMapMode", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::set_map_mode(mgr, m); }, 8});

    // ---- SetGraphicsMode(hdc, iMode) ----
    // Learn: returns previous mode, or 0 on failure. Must be stdcall 8 bytes.
    // A missing-export stub with 0 cleanup bytes corrupts the caller stack.
    exports.entries.push_back({"SetGraphicsMode", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::set_graphics_mode(mgr, m); }, 8});

    // ---- GetGraphicsMode(hdc) ----
    exports.entries.push_back({"GetGraphicsMode", 0, [&mgr](Marshaler& m) {
        const auto hdc = static_cast<Handle>(m.arg(0));
        auto it = mgr.m_dc_states.find(hdc);
        if (it == mgr.m_dc_states.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        m.set_return(static_cast<u32>(it->second.graphics_mode));
    }, 4});

    // ---- SetWorldTransform(hdc, lpxf) ----
    exports.entries.push_back({"SetWorldTransform", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::set_world_transform(mgr, m); }, 8});

    // ---- GetWorldTransform(hdc, lpxf) ----
    exports.entries.push_back({"GetWorldTransform", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::get_world_transform(mgr, m); }, 8});

    // ---- ModifyWorldTransform(hdc, lpxf, iMode) ----
    exports.entries.push_back({"ModifyWorldTransform", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::modify_world_transform(mgr, m); }, 12});

    // ---- SetPolyFillMode(hdc, iPolyFillMode) ----
    exports.entries.push_back({"SetPolyFillMode", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto mode = static_cast<i32>(m.arg(1));
        auto it = mgr.m_dc_states.find(hdc);
        i32 prev = 1; // ALTERNATE
        if (it != mgr.m_dc_states.end()) {
            prev = it->second.poly_fill_mode;
            it->second.poly_fill_mode = mode;
        }
        m.set_return(static_cast<u32>(prev));
    }, 8});

    // ---- GetDIBits(hdc, hbmp, uStartScan, cScanLines, lpvBits, lpbmi, uUsage) ----
    exports.entries.push_back({"GetDIBits", 0, [&mgr](Marshaler& m) {
        u32 error = 0;
        const i32 r = mgr.get_dib_bits(static_cast<Handle>(m.arg(0)), static_cast<Handle>(m.arg(1)),
                                       m.arg32(2), m.arg32(3), m.arg_ptr(4), m.arg_ptr(5),
                                       m.arg32(6), &error);
        if (r == 0 && error != 0) set_last_error(error);
        m.set_return(static_cast<u64>(static_cast<u32>(r)));
    }, 28});

    // ---- CreateBitmap(nWidth, nHeight, nPlanes, nBitCount, lpBits) ----
    exports.entries.push_back({"CreateBitmap", 0, [&mgr](Marshaler& m) {
        u32 error = 0;
        const Handle h = mgr.create_bitmap(static_cast<i32>(m.arg32(0)), static_cast<i32>(m.arg32(1)),
                                           m.arg32(2), m.arg32(3), m.arg_ptr(4), &error);
        if (h == 0 && error != 0) set_last_error(error);
        m.set_return(static_cast<u64>(h));
    }, 20});

    // ---- CreateBitmapIndirect(const BITMAP*) ----
    exports.entries.push_back({"CreateBitmapIndirect", 0, [&mgr](Marshaler& m) {
        const GuestAddr bm = m.arg_ptr(0);
        if (bm == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        const i32 w = static_cast<i32>(m.read_u32(bm + 4).value_or(0));
        const i32 h = static_cast<i32>(m.read_u32(bm + 8).value_or(0));
        const u32 planes = m.read_u16(bm + 16).value_or(1);
        const u32 bpp = m.read_u16(bm + 18).value_or(1);
        const GuestAddr bits = m.cpu_is_64bit() ? m.memory().read_u64(bm + 24).value_or(0)
                                                : m.read_u32(bm + 20).value_or(0);
        u32 error = 0;
        const Handle out = mgr.create_bitmap(w, h, planes, bpp, bits, &error);
        if (out == 0 && error != 0) set_last_error(error);
        m.set_return(static_cast<u64>(out));
    }, 4});

    // ---- GetCurrentObject(hdc, uObjectType) ----
    exports.entries.push_back({"GetCurrentObject", 0, [&mgr](Marshaler& m) {
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto type = static_cast<u32>(m.arg(1));
        // OBJ_PEN=1, OBJ_BRUSH=2, OBJ_PAL=5, OBJ_FONT=6, OBJ_BITMAP=7
        Handle result = 0;
        auto it = mgr.m_dc_states.find(hdc);
        if (it != mgr.m_dc_states.end()) {
            switch (type) {
            case 1: result = it->second.selected_pen; break;
            case 2: result = it->second.selected_brush; break;
            case 5: result = it->second.selected_palette; break;
            case 6: result = it->second.selected_font; break;
            case 7: result = it->second.selected_bitmap_handle; break;
            default: break;
            }
        }
        m.set_return(static_cast<u64>(result));
    }, 8});

    // ---- CreateDCA(lpszDriver, lpszDevice, lpszOutput, lpInitData) ----
    exports.entries.push_back({"CreateDCA", 0, [&mgr](Marshaler& m) {
        // Only the display is a device here: "DISPLAY" or \\.\DISPLAYn. A
        // printer name is ERROR_INVALID_PRINTER_NAME, as for a printer
        // Windows does not have ("CreateDCW(NoSuchPrinter)=NULL err=1801").
        const int kind = gdi_display_device(m, m.arg_ptr(0), false);
        if (kind <= 0) {
            if (kind < 0) set_last_error(error_codes::ERROR_INVALID_PRINTER_NAME);
            m.set_return(0u);
            return;
        }
        m.set_return(static_cast<u64>(mgr.create_ic_w()));
    }, 16});

    // ---- GetBkColor(hdc) ----
    exports.entries.push_back({"GetBkColor", 0, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto it = mgr.m_dc_states.find(hdc);
        if (it == mgr.m_dc_states.end()) {
            m.set_return(0xFFFFFFFFu); // CLR_INVALID
            return;
        }
        m.set_return(static_cast<u64>(it->second.bk_color));
    }, 4});
}

}  // namespace vitreo::winapi
