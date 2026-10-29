// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_viewport_region_exports.cpp
/// @brief Viewport, window-origin and region exports.
///
/// ExtTextOutW .. GetTextCharsetInfo: the "New GDI32 API stubs" block --
/// viewport and window origin/extent, region construction, pattern and
/// hatch brushes, font enumeration and charset translation. Reads
/// GdiManager::m_dc_states and m_region_objects directly.
///
/// One of the section registrars split out of make_gdi32_exports
/// (gdi32_registration.cpp); see gdi32_registration_internal.hpp. The entries
/// are pushed in their original order.

#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace vitreo::winapi {

void add_gdi32_viewport_region_exports(DllExports& exports, GdiManager& mgr) {
    // =========================================================================
    // New GDI32 API stubs (53 additional exports)
    // =========================================================================

    exports.entries.push_back({"ExtTextOutW", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::ext_text_out(mgr, m, true);
    }, 32});

    // ---- Escape(hdc, nEscape, cbInput, lpszInData, lpszOutData) ----
    exports.entries.push_back({"Escape", 0, [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
        (void)m.arg(3); (void)m.arg(4);
        m.set_return(0);
    }, 20});

    // ---- SetViewportOrgEx(hdc, x, y, lppt) ----
    exports.entries.push_back({"SetViewportOrgEx", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto x    = static_cast<i32>(m.arg(1));
        auto y    = static_cast<i32>(m.arg(2));
        auto lppt = m.arg_ptr(3);
        auto it = mgr.m_dc_states.find(hdc);
        if (it != mgr.m_dc_states.end()) {
            if (lppt) {
                (void)m.write_u32(lppt,     static_cast<u32>(it->second.viewport_org_x));
                (void)m.write_u32(lppt + 4, static_cast<u32>(it->second.viewport_org_y));
            }
            it->second.viewport_org_x = x;
            it->second.viewport_org_y = y;
        }
        m.set_return(1);
    }, 16});

    // ---- OffsetViewportOrgEx(hdc, x, y, lppt) ----
    exports.entries.push_back({"OffsetViewportOrgEx", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto x    = static_cast<i32>(m.arg(1));
        auto y    = static_cast<i32>(m.arg(2));
        auto lppt = m.arg_ptr(3);
        auto it = mgr.m_dc_states.find(hdc);
        if (it != mgr.m_dc_states.end()) {
            if (lppt) {
                (void)m.write_u32(lppt,     static_cast<u32>(it->second.viewport_org_x));
                (void)m.write_u32(lppt + 4, static_cast<u32>(it->second.viewport_org_y));
            }
            it->second.viewport_org_x += x;
            it->second.viewport_org_y += y;
        }
        m.set_return(1);
    }, 16});

    // ---- SetViewportExtEx(hdc, x, y, lpsize) ----
    exports.entries.push_back({"SetViewportExtEx", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::set_extent(mgr, m, true); }, 16});

    // ---- ScaleViewportExtEx(hdc, xNum, xDenom, yNum, yDenom, lpsize) ----
    exports.entries.push_back({"ScaleViewportExtEx", 0, [](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        (void)m.arg(1); (void)m.arg(2);
        (void)m.arg(3); (void)m.arg(4); (void)m.arg(5);
        m.set_return(1);
    }, 24});

    // ---- SetWindowOrgEx(hdc, x, y, lppt) ----
    exports.entries.push_back({"SetWindowOrgEx", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto x    = static_cast<i32>(m.arg(1));
        auto y    = static_cast<i32>(m.arg(2));
        auto lppt = m.arg_ptr(3);
        auto it = mgr.m_dc_states.find(hdc);
        if (it != mgr.m_dc_states.end()) {
            if (lppt) {
                (void)m.write_u32(lppt,     static_cast<u32>(it->second.window_org_x));
                (void)m.write_u32(lppt + 4, static_cast<u32>(it->second.window_org_y));
            }
            it->second.window_org_x = x;
            it->second.window_org_y = y;
        }
        m.set_return(1);
    }, 16});

    // ---- OffsetWindowOrgEx(hdc, x, y, lppt) ----
    exports.entries.push_back({"OffsetWindowOrgEx", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto x    = static_cast<i32>(m.arg(1));
        auto y    = static_cast<i32>(m.arg(2));
        auto lppt = m.arg_ptr(3);
        auto it = mgr.m_dc_states.find(hdc);
        if (it != mgr.m_dc_states.end()) {
            if (lppt) {
                (void)m.write_u32(lppt,     static_cast<u32>(it->second.window_org_x));
                (void)m.write_u32(lppt + 4, static_cast<u32>(it->second.window_org_y));
            }
            it->second.window_org_x += x;
            it->second.window_org_y += y;
        }
        m.set_return(1);
    }, 16});

    // ---- SetWindowExtEx(hdc, x, y, lpsize) ----
    exports.entries.push_back({"SetWindowExtEx", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::set_extent(mgr, m, false); }, 16});

    // ---- ScaleWindowExtEx(hdc, xNum, xDenom, yNum, yDenom, lpsize) ----
    exports.entries.push_back({"ScaleWindowExtEx", 0, [](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        (void)m.arg(1); (void)m.arg(2);
        (void)m.arg(3); (void)m.arg(4); (void)m.arg(5);
        m.set_return(1);
    }, 24});

    // ---- ExtSelectClipRgn(hdc, hrgn, fnMode) ----
    exports.entries.push_back({"ExtSelectClipRgn", 0, [&mgr](Marshaler& m) {
        auto hdc    = static_cast<Handle>(m.arg(0));
        auto hrgn   = static_cast<Handle>(m.arg(1));
        auto fnMode = static_cast<i32>(m.arg(2));
        auto result = mgr.ext_select_clip_rgn(hdc, hrgn, fnMode);
        m.set_return(static_cast<u32>(result));
    }, 12});

    // ---- CreatePatternBrush(hbmp) ----
    exports.entries.push_back({"CreatePatternBrush", 0, [&mgr](Marshaler& m) {
        const auto bitmap = static_cast<Handle>(m.arg(0));
        const auto h = mgr.create_pattern_brush(bitmap);
        VITREO_TRACE("[gdi] CreatePatternBrush bitmap=0x%x -> 0x%x\n",
                     bitmap, h);
        m.set_return(static_cast<u64>(h));
    }, 4});

    // ---- GetObjectType(hgdiobj) ----
    exports.entries.push_back({"GetObjectType", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.object_type(static_cast<Handle>(m.arg(0))));
    }, 4});

    // ---- CreateHatchBrush(iHatch, color) ----
    exports.entries.push_back({"CreateHatchBrush", 0, [&mgr](Marshaler& m) {
        m.set_return(static_cast<u64>(mgr.create_hatch_brush(m.arg32(0), m.arg32(1))));
    }, 8});

    // ---- SetRectRgn(hrgn, left, top, right, bottom) ----
    exports.entries.push_back({"SetRectRgn", 0, [&mgr](Marshaler& m) { m.set_return(mgr.set_rect_region(static_cast<Handle>(m.arg(0)), static_cast<i32>(m.arg32(1)), static_cast<i32>(m.arg32(2)), static_cast<i32>(m.arg32(3)), static_cast<i32>(m.arg32(4))) ? 1u : 0u); }, 20});

    // ---- CombineRgn(hrgnDest, hrgnSrc1, hrgnSrc2, fnCombineMode) ----
    exports.entries.push_back({"CombineRgn", 0, [&mgr](Marshaler& m) {
        auto hrgnDest = static_cast<Handle>(m.arg(0));
        auto hrgnSrc1 = static_cast<Handle>(m.arg(1));
        auto hrgnSrc2 = static_cast<Handle>(m.arg(2));
        auto mode     = static_cast<i32>(m.arg(3));

        auto s1 = mgr.m_region_objects.find(hrgnSrc1);
        auto s2 = mgr.m_region_objects.find(hrgnSrc2);

        if (mgr.m_region_objects.find(hrgnDest) == mgr.m_region_objects.end() ||
            s1 == mgr.m_region_objects.end() || mode < 1 || mode > 5 ||
            (mode != 5 && s2 == mgr.m_region_objects.end())) {
            m.set_return(0u);
            return;
        }
        const ClipRect result = ClipRect::combine(
            s1->second, mode == 5 ? ClipRect{} : s2->second, mode);
        mgr.m_region_objects[hrgnDest] = result;
        auto& dest = mgr.m_region_objects[hrgnDest];
        if (std::getenv("VITREO_INPUT_DIAG") != nullptr) {
            std::fprintf(stderr,
                "[rgn] CombineRgn dest=0x%x mode=%d -> (%d,%d,%d,%d)\n",
                static_cast<unsigned>(hrgnDest), mode,
                dest.left, dest.top, dest.right, dest.bottom);
        }
        m.set_return(dest.is_empty() ? NULLREGION : dest.rectangles.empty() ? SIMPLEREGION : COMPLEXREGION);
    }, 16});

    // ---- DPtoLP(hdc, lpPoints, nCount) ----
    exports.entries.push_back({"DPtoLP", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::convert_points(mgr, m, true); }, 12});

    // ---- SetDIBColorTable(hdc, iStart, cEntries, prgbq) ----
    exports.entries.push_back({"SetDIBColorTable", 0, [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
        m.set_return(0);
    }, 16});

    // ---- EnumFontFamiliesExW(hdc, lpLogfont, lpProc, lParam, dwFlags) ----
    exports.entries.push_back({"EnumFontFamiliesExW", 0, [&mgr](Marshaler& m) {
        // (hdc, lpLogfont, lpProc, lParam, dwFlags): font_enum.cpp. A NULL
        // LOGFONT enumerates every family ("NULL logfont ret=1").
        const GuestAddr lf = m.arg_ptr(1);
        GdiManager::FontInfo filter;
        filter.char_set = 1;  // DEFAULT_CHARSET
        if (lf != 0) filter = read_logfont(m, lf, true);
        m.set_return(static_cast<u32>(mgr.enum_font_families_ex(static_cast<Handle>(m.arg(0)), filter.face_name,
                                                                filter.char_set, m.arg_ptr(2), m.arg(3), true,
                                                                m.cpu_is_64bit())));
    }, 20});

    // ---- CreateEllipticRgn(left, top, right, bottom) ----
    exports.entries.push_back({"CreateEllipticRgn", 0, [&mgr](Marshaler& m) { m.set_return(static_cast<u64>(mgr.create_elliptic_region(static_cast<i32>(m.arg32(0)), static_cast<i32>(m.arg32(1)), static_cast<i32>(m.arg32(2)), static_cast<i32>(m.arg32(3))))); }, 16});

    // ---- CreatePolygonRgn(lppt, cCount, fnPolyFillMode) ----
    exports.entries.push_back({"CreatePolygonRgn", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::create_polygon_rgn(mgr, m, false); }, 12});

    // ---- Polyline(hdc, lppt, cCount) ----
    exports.entries.push_back({"Polyline", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::poly_points(mgr, m, gdi_geometry_exports::PolyKind::Polyline); }, 12});

    // ---- Polygon(hdc, lppt, cCount) ----
    exports.entries.push_back({"Polygon", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::poly_points(mgr, m, gdi_geometry_exports::PolyKind::Polygon); }, 12});

    // EnumFonts{A,W}(hdc, lpFaceName, lpProc, lParam). Learn: four arguments,
    // the callback receives a LOGFONT and TEXTMETRIC (the prefixes of the
    // structures EnumFontFamiliesEx already writes), and the return is the
    // last callback result. A NULL face enumerates one font per typeface.
    // A NULL DC fails with ERROR_INVALID_HANDLE. The generic missing-export
    // stub pops nothing and returns 0xFFFFFFFF, which is not this contract.
    // https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-enumfontsw
    auto enum_fonts = [&mgr](Marshaler& m, bool wide) {
        std::u16string face;
        if (const GuestAddr name = m.arg_ptr(1)) {
            const auto text = wide ? m.read_wide_string(name) : m.read_ansi_string(name);
            if (!text.is_ok()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const auto& bytes = text.value();
            std::vector<u16> wide_name(bytes.size() + 1);
            const auto converted = codepage::multi_byte_to_wide(
                wide ? codepage::kCpUtf8 : codepage::ansi_code_page(), 0,
                reinterpret_cast<const u8*>(bytes.data()), bytes.size(),
                wide_name.data(), wide_name.size());
            if (!converted.ok()) {
                set_last_error(converted.error);
                m.set_return(0);
                return;
            }
            usize count = converted.count;
            while (count > 0 && wide_name[count - 1] == 0) --count;
            // A present but empty name is not the NULL "one font per typeface"
            // request. An empty face would take that path, so keep a name no
            // family matches and return the documented no-callback result.
            face.assign(wide_name.data(), wide_name.data() + count);
            if (face.empty()) face.push_back(u'\uFFFF');
        }
        // DEFAULT_CHARSET: every charset the face claims, matching the
        // enumeration EnumFontFamiliesEx uses for an unspecified charset.
        m.set_return(static_cast<u32>(mgr.enum_font_families_ex(
            static_cast<Handle>(m.arg(0)), face, 1, m.arg_ptr(2), m.arg(3), wide,
            m.cpu_is_64bit())));
    };
    exports.entries.push_back({"EnumFontsW", 0, [enum_fonts](Marshaler& m) { enum_fonts(m, true); }, 16});
    exports.entries.push_back({"EnumFontsA", 0, [enum_fonts](Marshaler& m) { enum_fonts(m, false); }, 16});

    // ---- EnumFontFamiliesW(hdc, lpszFamily, lpProc, lParam) ----
    exports.entries.push_back({"EnumFontFamiliesW", 0, [](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
        m.set_return(1);
    }, 16});

    // ---- GetTextCharsetInfo(hdc, lpSig, dwFlags) ----
    exports.entries.push_back({"TranslateCharsetInfo", 0, [](Marshaler& m) {
        const u32 flags = static_cast<u32>(m.arg(2));
        if (m.arg_ptr(1) == 0 ||
            (flags != 1 && flags != 2 && flags != 3 && flags != 0x1000)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        } else {
            // A successful translation must supply both the code-page and
            // Unicode-subrange signature. No complete translation table is
            // installed yet; do not invent glyph coverage or modify outputs.
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            VITREO_TRACE("[gdi32] TranslateCharsetInfo: translation unavailable (flags=0x%x)\n", flags);
        }
        m.set_return(0);
    }, 12});

    exports.entries.push_back({"GetTextCharsetInfo", 0, [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
        m.set_return(1); // ANSI_CHARSET
    }, 12});
}

}  // namespace vitreo::winapi
