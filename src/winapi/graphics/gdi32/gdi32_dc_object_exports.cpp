// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_dc_object_exports.cpp
/// @brief Device-context and GDI-object lifetime exports.
///
/// GetDC .. AddFontMemResourceEx: DC acquisition, SelectObject and
/// DeleteObject, device caps, system metrics, SystemParametersInfoW and
/// LOGFONT-based font creation.
///
/// One of the section registrars split out of make_gdi32_exports
/// (gdi32_registration.cpp); see gdi32_registration_internal.hpp. The entries
/// are pushed in their original order.

#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

void add_gdi32_dc_object_exports(DllExports& exports, GdiManager& mgr) {
    exports.entries.push_back({"GetDC", 1, [&mgr](Marshaler& m) {
        auto hwnd = static_cast<Handle>(m.arg(0));
        auto h = mgr.get_dc(hwnd);
        m.set_return(static_cast<u64>(h));
    }, 4});
    exports.entries.push_back({"ReleaseDC", 2, [&mgr](Marshaler& m) {
        auto hwnd = static_cast<Handle>(m.arg(0));
        auto hdc  = static_cast<Handle>(m.arg(1));
        auto ret = mgr.release_dc(hwnd, hdc);
        m.set_return(static_cast<u64>(ret));
    }, 8});
    exports.entries.push_back({"CreateCompatibleDC", 3, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto h = mgr.create_compatible_dc(hdc);
        m.set_return(static_cast<u64>(h));
    }, 4});
    exports.entries.push_back({"DeleteDC", 4, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        // A DC already deleted: FALSE, last error untouched (s3gdi_bitmap_probe
        // "deletedc ... again").
        auto hdc = static_cast<Handle>(m.arg(0));
        bool ok = mgr.delete_dc(hdc);
        m.set_return(ok ? 1u : 0u);
    }, 4});
    exports.entries.push_back({"GetDeviceCaps", 5, [&mgr](Marshaler& m) {
        auto hdc   = static_cast<Handle>(m.arg(0));
        auto index = static_cast<i32>(m.arg(1));
        auto val = mgr.get_device_caps(hdc, index);
        m.set_return(static_cast<u64>(static_cast<u32>(val)));
    }, 8});
    exports.entries.push_back({"GetStockObject", 6, [&mgr](Marshaler& m) {
        auto id = static_cast<i32>(m.arg(0));
        auto h = mgr.get_stock_object(id);
        m.set_return(static_cast<u64>(h));
    }, 4});
    exports.entries.push_back({"SelectObject", 7, [&mgr](Marshaler& m) {
        u32 error = 0;
        auto prev = mgr.select_object(static_cast<Handle>(m.arg(0)),
                                      static_cast<Handle>(m.arg(1)), &error);
        if (prev == 0 && error != 0) set_last_error(error);
        m.set_return(static_cast<u64>(prev));
    }, 8});
    exports.entries.push_back({"DeleteObject", 8, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        auto obj = static_cast<Handle>(m.arg(0));
        bool ok = mgr.delete_object(obj);
        m.set_return(ok ? 1u : 0u);
    }, 4});
    exports.entries.push_back({"SetBkMode", 9, [&mgr](Marshaler& m) {
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto mode = static_cast<i32>(m.arg(1));
        auto prev = mgr.set_bk_mode(hdc, mode);
        if (std::getenv("VITREO_WIN_DIAG"))
            fprintf(stderr, "[gdi] SetBkMode hdc=0x%x mode=%d\n", (unsigned)hdc, mode);
        m.set_return(static_cast<u64>(static_cast<u32>(prev)));
    }, 8});
    exports.entries.push_back({"SetTextColor", 10, [&mgr](Marshaler& m) {
        auto hdc   = static_cast<Handle>(m.arg(0));
        auto color = static_cast<u32>(m.arg(1));
        auto prev = mgr.set_text_color(hdc, color);
        if (std::getenv("VITREO_WIN_DIAG"))
            fprintf(stderr, "[gdi] SetTextColor hdc=0x%x color=0x%06x\n", (unsigned)hdc, color);
        m.set_return(static_cast<u64>(prev));
    }, 8});
    exports.entries.push_back({"SetBkColor", 11, [&mgr](Marshaler& m) {
        auto hdc   = static_cast<Handle>(m.arg(0));
        auto color = static_cast<u32>(m.arg(1));
        auto prev = mgr.set_bk_color(hdc, color);
        if (std::getenv("VITREO_WIN_DIAG"))
            fprintf(stderr, "[gdi] SetBkColor hdc=0x%x color=0x%06x\n", (unsigned)hdc, color);
        m.set_return(static_cast<u64>(prev));
    }, 8});
    exports.entries.push_back({"GetSystemMetrics", 12, [&mgr](Marshaler& m) {
        auto index = static_cast<i32>(m.arg(0));
        auto val = mgr.get_system_metrics(index);
        m.set_return(static_cast<u64>(static_cast<u32>(val)));
    }, 4});
    exports.entries.push_back({"SystemParametersInfoW", 13, [&mgr](Marshaler& m) {
        // BOOL SystemParametersInfoW(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni)
        auto uiAction = static_cast<u32>(m.arg(0));
        auto uiParam  = static_cast<u32>(m.arg(1));
        auto pvParam  = m.arg_ptr(2);

        constexpr u32 SPI_GETWORKAREA = 0x0030;
        constexpr u32 SPI_GETNONCLIENTMETRICS = 0x0029;
        constexpr u32 SPI_GETICONTITLELOGFONT = 0x001F;
        constexpr u32 SPI_GETDRAGFULLWINDOWS = 0x0026;
        constexpr u32 SPI_GETFONTSMOOTHING = 0x004A;
        constexpr u32 SPI_GETFONTSMOOTHINGTYPE = 0x200A;
        constexpr u32 SPI_GETMINIMIZEDMETRICS = 0x002B;

        switch (uiAction) {
        case SPI_GETWORKAREA: {
            // Write a RECT (16 bytes): left, top, right, bottom
            if (pvParam) {
                i32 wl = 0, wt = 0, wr = 1920, wb = 1080;
                if (mgr.cocoa_bridge()) {
                    auto mi = mgr.cocoa_bridge()->get_primary_monitor_info();
                    wl = mi.work_left;
                    wt = mi.work_top;
                    wr = mi.work_right;
                    wb = mi.work_bottom;
                }
                (void)m.write_u32(pvParam +  0, static_cast<u32>(wl));
                (void)m.write_u32(pvParam +  4, static_cast<u32>(wt));
                (void)m.write_u32(pvParam +  8, static_cast<u32>(wr));
                (void)m.write_u32(pvParam + 12, static_cast<u32>(wb));
            }
            m.set_return(1); // TRUE
            break;
        }
        case SPI_GETNONCLIENTMETRICS: {
            // Mirror user32: NONCLIENTMETRICSW with real LOGFONTW heights.
            // Leaving lfHeight=0 made Qt emit QFont::setPointSizeF(0).
            if (!pvParam) {
                m.set_return(0);
                break;
            }
            u32 cb_size = m.read_u32(pvParam).value_or(0);
            if (cb_size == 0) {
                cb_size = uiParam;
            }
            constexpr u32 kNcmWXp = 500;
            constexpr u32 kNcmWVista = 504;
            if (cb_size != kNcmWXp && cb_size != kNcmWVista) {
                m.set_return(0);
                break;
            }
            std::vector<u8> buf(cb_size, 0);
            auto put_i32 = [&](size_t off, i32 v) {
                if (off + 4 <= buf.size()) {
                    std::memcpy(buf.data() + off, &v, 4);
                }
            };
            auto write_logfontw = [&](u32 off, i32 height, i32 weight) {
                put_i32(off + 0, height);
                put_i32(off + 16, weight);
                buf[off + 23] = 1; // DEFAULT_CHARSET
                // "Segoe UI"
                const char16_t face[] = u"Segoe UI";
                for (u32 i = 0; i < 32 && face[i]; ++i) {
                    const char16_t ch = face[i];
                    std::memcpy(buf.data() + off + 28 + i * 2, &ch, 2);
                }
            };
            put_i32(0, static_cast<i32>(cb_size));
            put_i32(4, 1);
            put_i32(8, 17);
            put_i32(12, 17);
            put_i32(16, 36);
            put_i32(20, 30);
            write_logfontw(24, -12, 700);
            put_i32(116, 15);
            put_i32(120, 15);
            write_logfontw(124, -11, 400);
            put_i32(216, 18);
            put_i32(220, 18);
            write_logfontw(224, -12, 400);
            write_logfontw(316, -12, 400);
            write_logfontw(408, -12, 400);
            if (cb_size >= kNcmWVista) {
                put_i32(500, 4);
            }
            (void)m.memory().write_bytes(pvParam, buf.data(), cb_size);
            m.set_return(1);
            break;
        }
        case SPI_GETDRAGFULLWINDOWS:
        case SPI_GETFONTSMOOTHING: {
            // Write BOOL result to pvParam
            if (pvParam) (void)m.write_u32(pvParam, 1);
            m.set_return(1);
            break;
        }
        case SPI_GETFONTSMOOTHINGTYPE: {
            // FE_FONTSMOOTHINGSTANDARD = 1
            if (pvParam) (void)m.write_u32(pvParam, 1);
            m.set_return(1);
            break;
        }
        case SPI_GETICONTITLELOGFONT: {
            // LOGFONTW: lfHeight at offset 0 (i32), lfWeight at offset 16 (i32)
            if (pvParam && uiParam >= 4) {
                std::vector<u8> buf(uiParam, 0);
                i32 lfHeight = -12;
                i32 lfWeight = 400;
                if (buf.size() >= 4)  std::memcpy(buf.data() + 0,  &lfHeight, 4);
                if (buf.size() >= 20) std::memcpy(buf.data() + 16, &lfWeight, 4);
                (void)m.memory().write_bytes(pvParam, buf.data(), uiParam);
            }
            m.set_return(1);
            break;
        }
        case SPI_GETMINIMIZEDMETRICS: {
            // MINIMIZEDMETRICS: cbSize(4) iWidth(4) iHorzGap(4) iVertGap(4) iArrange(4)
            if (pvParam && uiParam >= 4) {
                std::vector<u8> buf(uiParam, 0);
                auto put_i32 = [&](size_t off, i32 v) {
                    if (off + 4 <= buf.size()) std::memcpy(buf.data() + off, &v, 4);
                };
                put_i32(0,  20);   // cbSize
                put_i32(4,  160);  // iWidth
                put_i32(8,  0);    // iHorzGap
                put_i32(12, 0);    // iVertGap
                put_i32(16, 0);    // iArrange
                (void)m.memory().write_bytes(pvParam, buf.data(), uiParam);
            }
            m.set_return(1);
            break;
        }
        default:
            // Unknown SPI — return TRUE anyway (many apps just check BOOL result)
            m.set_return(1);
            break;
        }
    }, 16});
    exports.entries.push_back({"GetObjectW", 14, [&mgr](Marshaler& m) {
        auto hObj   = static_cast<Handle>(m.arg(0));
        auto cb     = static_cast<i32>(m.arg32(1));
        auto lpObj  = m.arg_ptr(2);
        u32 error = 0;
        auto result = mgr.get_object(hObj, cb, lpObj, m.cpu_is_64bit(), true, &error);
        if (result == 0 && error != 0) set_last_error(error);
        m.set_return(static_cast<u64>(static_cast<u32>(result)));
    }, 12});
    exports.entries.push_back({"CreateBrushIndirect", 15, [&mgr](Marshaler& m) {
        // LOGBRUSH { UINT lbStyle; COLORREF lbColor; ULONG_PTR lbHatch; }
        const GuestAddr lb = m.arg_ptr(0);
        if (lb == 0) { m.set_return(0); return; }
        const u32 style = m.read_u32(lb + 0).value_or(0xFFFFFFFFu);
        const u32 color = m.read_u32(lb + 4).value_or(0);
        const u64 hatch = m.cpu_is_64bit() ? m.memory().read_u64(lb + 8).value_or(0)
                                           : m.read_u32(lb + 8).value_or(0);
        Handle h = 0;
        switch (style) {
        case 0: h = mgr.create_solid_brush(color); break;                  // BS_SOLID
        case 1: h = mgr.get_stock_object(NULL_BRUSH_ID); break;            // BS_NULL
        case 2: h = mgr.create_hatch_brush(static_cast<u32>(hatch), color); break;  // BS_HATCHED
        case 3: case 7:                                                    // BS_PATTERN(8X8)
            h = mgr.create_pattern_brush(static_cast<Handle>(hatch));
            break;
        case 6: case 8:                                                    // BS_DIBPATTERNPT
            h = mgr.create_dib_pattern_brush(static_cast<GuestAddr>(hatch), color & 0xFFFFu);
            break;
        case 5: {                                                          // BS_DIBPATTERN (HGLOBAL)
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED gdi32!CreateBrushIndirect: "
                                     "BS_DIBPATTERN with an HGLOBAL\n");
            }
            break;
        }
        default: break;  // unknown style: NULL, last error untouched
        }
        m.set_return(static_cast<u64>(h));
    }, 4});
    exports.entries.push_back({"CreateFontIndirectW", 16, [&mgr](Marshaler& m) {
        const GuestAddr lf = m.arg_ptr(0);  // NULL: NULL, last error untouched
        m.set_return(lf == 0 ? 0u : static_cast<u64>(mgr.create_font(read_logfont(m, lf, true))));
    }, 4});

    // ---- CreateFontIndirectA(LOGFONTA*) ----
    // PuTTY (ANSI) makes its bold panel-title font via this.
    exports.entries.push_back({"CreateFontIndirectA", 0, [&mgr](Marshaler& m) {
        const GuestAddr lf = m.arg_ptr(0);
        m.set_return(lf == 0 ? 0u : static_cast<u64>(mgr.create_font(read_logfont(m, lf, false))));
    }, 4});

    // ---- GetTextExtentPointA(hdc, lpString, c, lpSize) ----
    // The text exports below are marshalled in text_exports.cpp (contracts
    // measured by s3gdi_text_probe / s3gdi_otm_probe) and measured and drawn
    // by the text engine (text_engine.cpp).
    exports.entries.push_back({"GetTextExtentPointA", 0, [&mgr](Marshaler& m) { gdi_text_exports::get_text_extent_point(mgr, m, false); }, 16});

    // ---- AddFontMemResourceEx(font_data, font_data_size, reserved, num_fonts_ptr) ----
    exports.entries.push_back({"AddFontMemResourceEx", 17, [&mgr](Marshaler& m) { gdi_text_exports::add_font_mem_resource_ex(mgr, m); }, 16});
}

}  // namespace vitreo::winapi
