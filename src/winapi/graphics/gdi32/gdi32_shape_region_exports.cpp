// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_shape_region_exports.cpp
/// @brief Shape and region-query exports.
///
/// Rectangle .. GetWindowOrgEx: Rectangle, RoundRect, region boxes and
/// hit tests, region fills, coordinate conversion and colour lookups.
/// Reads GdiManager::m_dc_states, m_pen_objects, m_brush_colors and
/// m_region_objects directly.
///
/// One of the section registrars split out of make_gdi32_exports
/// (gdi32_registration.cpp); see gdi32_registration_internal.hpp. The entries
/// are pushed in their original order.

#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/kernel32/error.hpp"

#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi {

void add_gdi32_shape_region_exports(DllExports& exports, GdiManager& mgr) {
    // ---- Rectangle(hdc, left, top, right, bottom) ----
    exports.entries.push_back({"Rectangle", 0, [&mgr](Marshaler& m) {
        auto hdc    = static_cast<Handle>(m.arg(0));
        if (hdc == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto left   = static_cast<i32>(m.arg(1));
        auto top    = static_cast<i32>(m.arg(2));
        auto right  = static_cast<i32>(m.arg(3));
        auto bottom = static_cast<i32>(m.arg(4));
        if (mgr.path_record_rect(hdc, left, top, right, bottom)) { m.set_return(1u); return; }
        auto dc_it = mgr.m_dc_states.find(hdc);
        if (dc_it != mgr.m_dc_states.end()) {
            // Draw pen outline (1px border) if pen is not PS_NULL
            auto pen_it = mgr.m_pen_objects.find(dc_it->second.selected_pen);
            bool has_pen = pen_it != mgr.m_pen_objects.end() && pen_it->second.style != 5;
            if (has_pen) {
                u32 pc = pen_it->second.color;
                mgr.fill_rect_gdi(hdc, left, top, right, top + 1, pc);      // top
                mgr.fill_rect_gdi(hdc, left, bottom - 1, right, bottom, pc); // bottom
                mgr.fill_rect_gdi(hdc, left, top, left + 1, bottom, pc);     // left
                mgr.fill_rect_gdi(hdc, right - 1, top, right, bottom, pc);   // right
            }
            // Fill interior with brush (inset by pen width)
            i32 inset = has_pen ? 1 : 0;
            u32 brush_color = 0x00FFFFFF;
            auto br_it = mgr.m_brush_colors.find(dc_it->second.selected_brush);
            if (br_it != mgr.m_brush_colors.end()) {
                brush_color = br_it->second;
                mgr.fill_rect_gdi(hdc, left + inset, top + inset,
                                  right - inset, bottom - inset, brush_color);
            }
            // If brush is NULL_BRUSH (no entry in brush_colors), skip fill
        }
        m.set_return(1);
    }, 20});

    // ---- CreateRoundRectRgn(left, top, right, bottom, cx, cy) ----
    exports.entries.push_back({"CreateRoundRectRgn", 0, [&mgr](Marshaler& m) { m.set_return(static_cast<u64>(mgr.create_round_rect_region(static_cast<i32>(m.arg32(0)), static_cast<i32>(m.arg32(1)), static_cast<i32>(m.arg32(2)), static_cast<i32>(m.arg32(3)), static_cast<i32>(m.arg32(4)), static_cast<i32>(m.arg32(5))))); }, 24});

    // ---- OffsetRgn(hrgn, x, y) ----
    exports.entries.push_back({"OffsetRgn", 0, [&mgr](Marshaler& m) {
        auto hrgn = static_cast<Handle>(m.arg(0));
        auto x    = static_cast<i32>(m.arg(1));
        auto y    = static_cast<i32>(m.arg(2));
        auto it = mgr.m_region_objects.find(hrgn);
        if (it == mgr.m_region_objects.end()) { m.set_return(0u); return; }
        if (!it->second.is_empty()) {
            for (auto& r : it->second.rectangles) {
                r[0] += x; r[2] += x; r[1] += y; r[3] += y;
            }
            it->second.left += x;
            it->second.right += x;
            it->second.top += y;
            it->second.bottom += y;
        }
        m.set_return(it->second.is_empty() ? NULLREGION : it->second.rectangles.empty() ? SIMPLEREGION : COMPLEXREGION);
    }, 12});

    // ---- GetRgnBox(hrgn, lprc) ----
    exports.entries.push_back({"GetRgnBox", 0, [&mgr](Marshaler& m) {
        auto hrgn = static_cast<Handle>(m.arg(0));
        auto lprc = m.arg_ptr(1);
        auto it = mgr.m_region_objects.find(hrgn);
        if (it == mgr.m_region_objects.end() || !lprc) { m.set_return(0u); return; }
        {
            (void)m.write_u32(lprc,      static_cast<u32>(it->second.left));
            (void)m.write_u32(lprc + 4,  static_cast<u32>(it->second.top));
            (void)m.write_u32(lprc + 8,  static_cast<u32>(it->second.right));
            (void)m.write_u32(lprc + 12, static_cast<u32>(it->second.bottom));
        }
        m.set_return(it->second.is_empty() ? NULLREGION : it->second.rectangles.empty() ? SIMPLEREGION : COMPLEXREGION);
    }, 8});

    // ---- GetRegionData(hrgn, dwCount, lpRgnData) ----
    exports.entries.push_back({"GetRegionData", 0, [&mgr](Marshaler& m) {
        auto hrgn = static_cast<Handle>(m.arg(0));
        auto count = static_cast<u32>(m.arg(1));
        auto data = m.arg_ptr(2);
        auto it = mgr.m_region_objects.find(hrgn);
        if (it == mgr.m_region_objects.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0u);
            return;
        }

        const bool empty = it->second.is_empty();
        const auto rectangles = it->second.parts();
        const u32 rect_bytes = static_cast<u32>(rectangles.size()) * 16u;
        const u32 required = 32u + rect_bytes;
        if (data == 0) {
            m.set_return(required);
            return;
        }
        if (count < required) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }

        (void)m.write_u32(data + 0, 32u);          // dwSize
        (void)m.write_u32(data + 4, 1u);           // RDH_RECTANGLES
        (void)m.write_u32(data + 8, static_cast<u32>(rectangles.size()));
        (void)m.write_u32(data + 12, rect_bytes);
        (void)m.write_u32(data + 16, empty ? 0u : static_cast<u32>(it->second.left));
        (void)m.write_u32(data + 20, empty ? 0u : static_cast<u32>(it->second.top));
        (void)m.write_u32(data + 24, empty ? 0u : static_cast<u32>(it->second.right));
        (void)m.write_u32(data + 28, empty ? 0u : static_cast<u32>(it->second.bottom));
        for (size_t i = 0; i < rectangles.size(); ++i)
            for (size_t j = 0; j < 4; ++j)
                (void)m.write_u32(data + 32 + i * 16 + j * 4,
                                  static_cast<u32>(rectangles[i][j]));
        m.set_return(required);
    }, 12});

    // ---- RectInRegion(hrgn, lprc) ----
    exports.entries.push_back({"RectInRegion", 0, [&mgr](Marshaler& m) {
        auto hrgn = static_cast<Handle>(m.arg(0));
        auto rect = m.arg_ptr(1);
        auto it = mgr.m_region_objects.find(hrgn);
        if (it == mgr.m_region_objects.end() || rect == 0 || it->second.is_empty()) {
            m.set_return(0u);
            return;
        }
        const i32 left = static_cast<i32>(m.read_u32(rect + 0).value_or(0));
        const i32 top = static_cast<i32>(m.read_u32(rect + 4).value_or(0));
        const i32 right = static_cast<i32>(m.read_u32(rect + 8).value_or(0));
        const i32 bottom = static_cast<i32>(m.read_u32(rect + 12).value_or(0));
        bool intersects = false;
        if (left < right && top < bottom)
            for (const auto& r : it->second.parts())
                intersects |= left < r[2] && right > r[0] && top < r[3] && bottom > r[1];
        m.set_return(intersects ? 1u : 0u);
    }, 8});

    // ---- RoundRect(hdc, left, top, right, bottom, cx, cy) ----
    exports.entries.push_back({"RoundRect", 0, [&mgr](Marshaler& m) {
        auto hdc    = static_cast<Handle>(m.arg(0));
        if (hdc == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto left   = static_cast<i32>(m.arg(1));
        auto top    = static_cast<i32>(m.arg(2));
        auto right  = static_cast<i32>(m.arg(3));
        auto bottom = static_cast<i32>(m.arg(4));
        if (mgr.path_record_round_rect(hdc, left, top, right, bottom, static_cast<i32>(m.arg32(5)),
                                       static_cast<i32>(m.arg32(6)))) {
            m.set_return(1u);
            return;
        }
        (void)m.arg(5); (void)m.arg(6); // cx, cy — no rounding implemented
        auto dc_it = mgr.m_dc_states.find(hdc);
        if (dc_it != mgr.m_dc_states.end()) {
            // Draw pen outline (same as Rectangle)
            auto pen_it = mgr.m_pen_objects.find(dc_it->second.selected_pen);
            bool has_pen = pen_it != mgr.m_pen_objects.end() && pen_it->second.style != 5;
            if (has_pen) {
                u32 pc = pen_it->second.color;
                mgr.fill_rect_gdi(hdc, left, top, right, top + 1, pc);
                mgr.fill_rect_gdi(hdc, left, bottom - 1, right, bottom, pc);
                mgr.fill_rect_gdi(hdc, left, top, left + 1, bottom, pc);
                mgr.fill_rect_gdi(hdc, right - 1, top, right, bottom, pc);
            }
            i32 inset = has_pen ? 1 : 0;
            u32 brush_color = 0x00FFFFFF;
            auto br_it = mgr.m_brush_colors.find(dc_it->second.selected_brush);
            if (br_it != mgr.m_brush_colors.end()) {
                brush_color = br_it->second;
                mgr.fill_rect_gdi(hdc, left + inset, top + inset,
                                  right - inset, bottom - inset, brush_color);
            }
        }
        m.set_return(1);
    }, 28});

    // ---- ExtFloodFill(hdc, x, y, color, fuFillType) ----
    exports.entries.push_back({"ExtFloodFill", 0, [](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        (void)m.arg(1); (void)m.arg(2);
        (void)m.arg(3); (void)m.arg(4);
        m.set_return(1);
    }, 20});

    // ---- GetViewportOrgEx(hdc, lppt) ----
    exports.entries.push_back({"GetViewportOrgEx", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto lppt = m.arg_ptr(1);
        if (lppt) {
            auto it = mgr.m_dc_states.find(hdc);
            i32 ox = 0, oy = 0;
            if (it != mgr.m_dc_states.end()) {
                ox = it->second.viewport_org_x;
                oy = it->second.viewport_org_y;
            }
            (void)m.write_u32(lppt,     static_cast<u32>(ox));
            (void)m.write_u32(lppt + 4, static_cast<u32>(oy));
        }
        m.set_return(1);
    }, 8});

    // ---- LPtoDP(hdc, lpPoints, nCount) ----
    exports.entries.push_back({"LPtoDP", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::convert_points(mgr, m, false); }, 12});

    // ---- PtInRegion(hrgn, x, y) ----
    exports.entries.push_back({"PtInRegion", 0, [&mgr](Marshaler& m) {
        auto hrgn = static_cast<Handle>(m.arg(0));
        auto x    = static_cast<i32>(m.arg(1));
        auto y    = static_cast<i32>(m.arg(2));
        auto it = mgr.m_region_objects.find(hrgn);
        bool inside = false;
        if (it != mgr.m_region_objects.end()) {
            inside = it->second.contains(x, y);
        }
        if (std::getenv("VITREO_INPUT_DIAG") != nullptr) {
            std::fprintf(stderr,
                "[rgn] PtInRegion hrgn=0x%x pt=(%d,%d) known=%d rect=(%d,%d,%d,%d) -> %d\n",
                static_cast<unsigned>(hrgn), x, y,
                it != mgr.m_region_objects.end() ? 1 : 0,
                it != mgr.m_region_objects.end() ? it->second.left : 0,
                it != mgr.m_region_objects.end() ? it->second.top : 0,
                it != mgr.m_region_objects.end() ? it->second.right : 0,
                it != mgr.m_region_objects.end() ? it->second.bottom : 0,
                inside ? 1 : 0);
        }
        m.set_return(inside ? 1u : 0u);
    }, 12});

    // ---- FillRgn(hdc, hrgn, hbr) ----
    exports.entries.push_back({"FillRgn", 0, [](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
        m.set_return(1);
    }, 12});

    // ---- FrameRgn(hdc, hrgn, hbr, nWidth, nHeight) ----
    exports.entries.push_back({"FrameRgn", 0, [](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        (void)m.arg(1); (void)m.arg(2);
        (void)m.arg(3); (void)m.arg(4);
        m.set_return(1);
    }, 20});

    // ---- GetBoundsRect(hdc, lprcBounds, flags) ----
    exports.entries.push_back({"GetBoundsRect", 0, [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
        m.set_return(0);
    }, 12});

    // ---- GetNearestPaletteIndex(hPal, crColor) ----
    exports.entries.push_back({"GetNearestColor", 0, [&mgr](Marshaler& m) {
        const Handle dc = static_cast<Handle>(m.arg(0));
        const auto found = mgr.m_dc_states.find(dc);
        if (found == mgr.m_dc_states.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0xFFFFFFFFu); // CLR_INVALID
            return;
        }
        const u32 color = static_cast<u32>(m.arg(1));
        if ((color & 0xFF000000u) != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        // The runtime's GDI device uses RGBA8 surfaces, so every RGB color
        // is representable without palette quantization.
        m.set_return(color);
    }, 8});

    exports.entries.push_back({"GetNearestPaletteIndex", 0, [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1);
        m.set_return(0);
    }, 8});

    // ---- GetWindowOrgEx(hdc, lppt) ----
    exports.entries.push_back({"GetWindowOrgEx", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto lppt = m.arg_ptr(1);
        if (lppt) {
            auto it = mgr.m_dc_states.find(hdc);
            i32 ox = 0, oy = 0;
            if (it != mgr.m_dc_states.end()) {
                ox = it->second.window_org_x;
                oy = it->second.window_org_y;
            }
            (void)m.write_u32(lppt,     static_cast<u32>(ox));
            (void)m.write_u32(lppt + 4, static_cast<u32>(oy));
        }
        m.set_return(1);
    }, 8});
}

}  // namespace vitreo::winapi
