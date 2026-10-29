// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_drawing_exports.cpp
/// @brief Pixel-format, bitmap and basic drawing exports.
///
/// ChoosePixelFormat .. StretchBlt: pixel formats, DIB sections, text
/// output, pens, brushes, BitBlt, PatBlt, StretchBlt and the text metric
/// queries.
///
/// One of the section registrars split out of make_gdi32_exports
/// (gdi32_registration.cpp); see gdi32_registration_internal.hpp. The entries
/// are pushed in their original order.

#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/graphics/gdi32/pixel_format_api.hpp"
#include "winapi/graphics/opengl32/gl_pixel_formats.hpp"
#include "winapi/kernel32/error.hpp"

#include <cstdlib>

namespace vitreo::winapi {

void add_gdi32_drawing_exports(DllExports& exports, GdiManager& mgr) {
    // ---- ChoosePixelFormat(hdc, pfd) ----
    // https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-choosepixelformat
    exports.entries.push_back({"ChoosePixelFormat", 18, [&mgr](Marshaler& m) { pixel_format_choose(mgr, m); }, 8});

    // ---- CreateDIBSection(hdc, bmi, usage, bits_ptr, section, offset) ----
    exports.entries.push_back({"CreateDIBSection", 19, [&mgr](Marshaler& m) {
        u32 error = 0;
        const Handle h = mgr.create_dib_section(static_cast<Handle>(m.arg(0)), m.arg_ptr(1),
                                                m.arg32(2), m.arg_ptr(3), m.cpu_is_64bit(),
                                                m.arg_ptr(4), m.arg32(5), &error);
        if (h == 0 && error != 0) set_last_error(error);
        m.set_return(static_cast<u64>(h));
    }, 24});

    // ---- CreateFontW(14 params) ----
    exports.entries.push_back({"CreateFontW", 20, [&mgr](Marshaler& m) {
        m.set_return(static_cast<u64>(mgr.create_font(read_create_font_args(m, true))));
    }, 56});

    // ---- CreateICW(driver, device, output, init_data) ----
    exports.entries.push_back({"CreateICW", 21, [&mgr](Marshaler& m) {
        // Only the display is a device here: "DISPLAY" or \\.\DISPLAYn. A
        // printer name is ERROR_INVALID_PRINTER_NAME, as for a printer
        // Windows does not have ("CreateDCW(NoSuchPrinter)=NULL err=1801").
        const int kind = gdi_display_device(m, m.arg_ptr(0), true);
        if (kind <= 0) {
            if (kind < 0) set_last_error(error_codes::ERROR_INVALID_PRINTER_NAME);
            m.set_return(0u);
            return;
        }
        m.set_return(static_cast<u64>(mgr.create_ic_w()));
    }, 16});

    // ---- CreateICA — ANSI CreateIC (Learn wingdi CreateICA) ----
    // Same contract as CreateICW: information context for device-caps queries.
    // ISRT.dll resolves this by name during DllMain; missing export aborts load.
    exports.entries.push_back({"CreateICA", 0, [&mgr](Marshaler& m) {
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

    // ---- GetTextExtentPoint32W(hdc, string, count, size_ptr) ----
    exports.entries.push_back({"GetTextExtentPoint32W", 22, [&mgr](Marshaler& m) { gdi_text_exports::get_text_extent_point(mgr, m, true); }, 16});

    // ---- RemoveFontMemResourceEx(handle) ----
    exports.entries.push_back({"RemoveFontMemResourceEx", 23, [&mgr](Marshaler& m) { gdi_text_exports::remove_font_mem_resource_ex(mgr, m); }, 4});

    // ---- SetPixelFormat(hdc, format, pfd) ----
    exports.entries.push_back({"SetPixelFormat", 24, [&mgr](Marshaler& m) { pixel_format_set(mgr, m); }, 12});

    // ---- SwapBuffers(hdc) ----
    exports.entries.push_back({"SwapBuffers", 25, [&mgr](Marshaler& m) {
        // A NULL DC, or one whose window has no pixel format, fails with
        // ERROR_INVALID_HANDLE (measured: last_error_on_success_user_probe,
        // and opengl32!wglSwapBuffers -- which SwapBuffers calls -- in
        // gfx_media_exports_probe.expected).
        const auto hdc = static_cast<Handle>(m.arg(0));
        // opengl32 owns the swap: the window's GL drawable exchanges and
        // presents (Opengl32Manager::swap_buffers, the same rule).
        if (const auto& swap = mgr.swap_buffers_handler()) {
            m.set_return(swap(hdc) ? 1 : 0);
            return;
        }
        const i32 index = hdc != 0 ? mgr.pixel_format(hdc) : 0;
        const gl::GlPixelFormat* format = index != 0 ? gl::gl_pixel_format(index) : nullptr;
        if (format == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        // A single-buffered format has no back buffer: FALSE with the last
        // error untouched (wgl_contract_probe64.expected swap-buffers-no-ctx-single).
        // A DC without a window succeeds (swap-buffers-memdc ret=1 gle=untouched).
        m.set_return(format->double_buffer || mgr.window_from_dc(hdc) == 0 ? 1 : 0);
    }, 4});

    // ---- TextOutW(hdc, x, y, string, count) ----
    exports.entries.push_back({"TextOutW", 26, [&mgr](Marshaler& m) { gdi_text_exports::text_out(mgr, m, true); }, 20});

    // ---- TextOutA(hdc, x, y, lpString, count) ----
    // ANSI sibling of TextOutW. PuTTY (an ANSI app) draws its config-dialog panel
    // title via TextOutA; without this it was a missing-export stub, so the
    // painted title never appeared.
    exports.entries.push_back({"TextOutA", 0, [&mgr](Marshaler& m) { gdi_text_exports::text_out(mgr, m, false); }, 20});

    // ---- SetROP2(hdc, rop2) ----
    exports.entries.push_back({"SetROP2", 27, [&mgr](Marshaler& m) {
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto mode = static_cast<i32>(m.arg(1));
        auto prev = mgr.set_rop2(hdc, mode);
        m.set_return(static_cast<u64>(static_cast<u32>(prev)));
    }, 8});

    // ---- GetLayout(hdc) ----
    exports.entries.push_back({"GetLayout", 28, [](Marshaler& m) {
        (void)m.arg(0); // hdc
        m.set_return(0u); // LAYOUT_LTR (left-to-right) — all DCs default LTR
    }, 4});

    // ---- SetLayout(hdc, layout) ----
    exports.entries.push_back({"SetLayout", 29, [](Marshaler& m) {
        (void)m.arg(0); // hdc
        (void)m.arg(1); // layout
        m.set_return(0u); // previous layout (always LTR)
    }, 8});

    // ---- LineTo(hdc, x, y) ----
    exports.entries.push_back({"LineTo", 30, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto x   = static_cast<i32>(m.arg(1));
        auto y   = static_cast<i32>(m.arg(2));
        auto ok  = mgr.line_to(hdc, x, y);
        m.set_return(ok ? 1u : 0u);
    }, 12});

    // ---- CreatePen(style, width, color) ----
    exports.entries.push_back({"CreatePen", 31, [&mgr](Marshaler& m) {
        auto style = static_cast<i32>(m.arg(0));
        auto width = static_cast<i32>(m.arg(1));
        auto color = static_cast<u32>(m.arg(2));
        auto h = mgr.create_pen(style, width, color);
        m.set_return(static_cast<u64>(h));
    }, 12});

    // CreatePenIndirect(const LOGPEN*). Learn: one pointer argument, NULL on
    // failure. LOGPEN is lopnStyle, POINT lopnWidth (x is the width; y is
    // unused), lopnColor. A NULL pointer fails and does not invent a last
    // error the page does not name.
    // https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createpenindirect
    // https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-logpen
    exports.entries.push_back({"CreatePenIndirect", 0, [&mgr](Marshaler& m) {
        const GuestAddr pen = m.arg_ptr(0);
        if (pen == 0) {
            m.set_return(0);
            return;
        }
        const auto style = m.read_u32(pen);
        const auto width = m.read_u32(pen + 4);
        const auto color = m.read_u32(pen + 12);
        if (!style.is_ok() || !width.is_ok() || !color.is_ok()) {
            m.set_return(0);
            return;
        }
        m.set_return(static_cast<u64>(mgr.create_pen(
            static_cast<i32>(style.value()), static_cast<i32>(width.value()), color.value())));
    }, 4});

    // ---- CreateCompatibleBitmap(hdc, width, height) ----
    exports.entries.push_back({"CreateCompatibleBitmap", 32, [&mgr](Marshaler& m) {
        // NULL or a stale DC: NULL with the last error untouched
        // (s3gdi_bitmap_probe "ccb NULL dc").
        auto hdc    = static_cast<Handle>(m.arg(0));
        auto width  = static_cast<i32>(m.arg32(1));
        auto height = static_cast<i32>(m.arg32(2));
        auto h = mgr.create_compatible_bitmap(hdc, width, height);
        if (h == 0 && hdc != 0 && (width < 0 || height < 0)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        }
        m.set_return(static_cast<u64>(h));
    }, 12});

    // ---- SetDIBitsToDevice(hdc, xDest, yDest, w, h, xSrc, ySrc, startScan, lines, bits, bmi, usage) ----
    exports.entries.push_back({"SetDIBitsToDevice", 33, [&mgr](Marshaler& m) {
        auto hdc       = static_cast<Handle>(m.arg(0));
        auto xDest     = static_cast<i32>(m.arg(1));
        auto yDest     = static_cast<i32>(m.arg(2));
        auto w         = static_cast<u32>(m.arg(3));
        auto h         = static_cast<u32>(m.arg(4));
        auto xSrc      = static_cast<i32>(m.arg(5));
        auto ySrc      = static_cast<i32>(m.arg(6));
        auto startScan = static_cast<u32>(m.arg(7));
        auto cLines    = static_cast<u32>(m.arg(8));
        auto bits      = static_cast<GuestAddr>(m.arg(9));
        auto bmi       = static_cast<GuestAddr>(m.arg(10));
        auto usage     = static_cast<u32>(m.arg(11));
        auto ret = mgr.set_dib_bits_to_device(hdc, xDest, yDest, w, h,
                                               xSrc, ySrc, startScan, cLines,
                                               bits, bmi, usage);
        m.set_return(static_cast<u64>(static_cast<u32>(ret)));
    }, 48});

    // ---- SetDIBits(hdc, hbm, start, lines, bits, bmi, usage) ----
    exports.entries.push_back({"SetDIBits", 0, [&mgr](Marshaler& m) {
        auto hdc       = static_cast<Handle>(m.arg(0));
        auto hbm       = static_cast<Handle>(m.arg(1));
        auto startScan = static_cast<u32>(m.arg(2));
        auto cLines    = static_cast<u32>(m.arg(3));
        auto bits      = static_cast<GuestAddr>(m.arg(4));
        auto bmi       = static_cast<GuestAddr>(m.arg(5));
        auto usage     = static_cast<u32>(m.arg(6));
        auto ret = mgr.set_dib_bits(hdc, hbm, startScan, cLines, bits, bmi,
                                    usage);
        m.set_return(static_cast<u64>(static_cast<u32>(ret)));
    }, 28});

    // ---- GdiAlphaBlend — gdi32 alias of msimg32!AlphaBlend (same ABI) ----
    exports.entries.push_back({"GdiAlphaBlend", 0, [&mgr](Marshaler& m) {
        const u32 blend = m.arg32(10);  // BLENDFUNCTION by value
        const u32 error = mgr.alpha_blend_checked(
            static_cast<Handle>(m.arg(0)), static_cast<i32>(m.arg32(1)),
            static_cast<i32>(m.arg32(2)), static_cast<i32>(m.arg32(3)),
            static_cast<i32>(m.arg32(4)), static_cast<Handle>(m.arg(5)),
            static_cast<i32>(m.arg32(6)), static_cast<i32>(m.arg32(7)),
            static_cast<i32>(m.arg32(8)), static_cast<i32>(m.arg32(9)),
            static_cast<u8>(blend & 0xFF), static_cast<u8>((blend >> 16) & 0xFF),
            static_cast<u8>((blend >> 24) & 0xFF));
        if (error != 0 && error != 0xFFFFFFFFu) set_last_error(error);
        m.set_return(error == 0 ? 1u : 0u);
    }, 44});

    // ---- GdiGradientFill / GdiTransparentBlt — the gdi32 entry points that
    // msimg32!GradientFill / TransparentBlt forward to (same ABI and
    // contract), on the same implementation msimg32 uses.
    exports.entries.push_back({"GdiGradientFill", 0, [&mgr](Marshaler& m) {
        const u32 result = mgr.gradient_fill(static_cast<Handle>(m.arg(0)), m.arg_ptr(1),
                                             m.arg32(2), m.arg_ptr(3), m.arg32(4), m.arg32(5));
        if (result != 0 && result != 0xFFFFFFFFu) set_last_error(result);
        m.set_return(result == 0 ? 1u : 0u);
    }, 24});
    exports.entries.push_back({"GdiTransparentBlt", 0, [&mgr](Marshaler& m) {
        const bool ok = mgr.transparent_blt(
            static_cast<Handle>(m.arg(0)), static_cast<i32>(m.arg32(1)), static_cast<i32>(m.arg32(2)),
            static_cast<i32>(m.arg32(3)), static_cast<i32>(m.arg32(4)), static_cast<Handle>(m.arg(5)),
            static_cast<i32>(m.arg32(6)), static_cast<i32>(m.arg32(7)), static_cast<i32>(m.arg32(8)),
            static_cast<i32>(m.arg32(9)), m.arg32(10));
        m.set_return(ok ? 1u : 0u);
    }, 44});

    // ---- MoveToEx(hdc, x, y, lppt) ----
    exports.entries.push_back({"MoveToEx", 34, [&mgr](Marshaler& m) {
        auto hdc  = static_cast<Handle>(m.arg(0));
        auto x    = static_cast<i32>(m.arg(1));
        auto y    = static_cast<i32>(m.arg(2));
        auto lppt = m.arg_ptr(3);
        i32 prev_x = 0, prev_y = 0;
        auto ok = mgr.move_to_ex(hdc, x, y, &prev_x, &prev_y);
        if (lppt) {
            (void)m.write_u32(lppt,     static_cast<u32>(prev_x));
            (void)m.write_u32(lppt + 4, static_cast<u32>(prev_y));
        }
        m.set_return(ok ? 1u : 0u);
    }, 16});

    // ---- GetCurrentPositionEx(hdc, lppt): the pen position MoveToEx,
    // LineTo and TA_UPDATECP text move (s3gdi_textdraw_probe "updatecp after").
    exports.entries.push_back({"GetCurrentPositionEx", 0, [&mgr](Marshaler& m) {
        const auto hdc = static_cast<Handle>(m.arg(0));
        const GuestAddr lppt = m.arg_ptr(1);
        i32 x = 0, y = 0;
        if (!mgr.current_position(hdc, x, y) || lppt == 0) {
            m.set_return(0u);
            return;
        }
        (void)m.write_u32(lppt, static_cast<u32>(x));
        (void)m.write_u32(lppt + 4, static_cast<u32>(y));
        m.set_return(1u);
    }, 8});

    // ---- SetPixel(hdc, x, y, color) ----
    exports.entries.push_back({"SetPixel", 35, [&mgr](Marshaler& m) {
        auto hdc   = static_cast<Handle>(m.arg(0));
        auto x     = static_cast<i32>(m.arg(1));
        auto y     = static_cast<i32>(m.arg(2));
        auto color = static_cast<u32>(m.arg(3));
        auto ret   = mgr.set_pixel(hdc, x, y, color);
        m.set_return(static_cast<u64>(ret));
    }, 16});

    // ---- BitBlt(hdcDest, x, y, cx, cy, hdcSrc, x1, y1, rop) ----
    exports.entries.push_back({"BitBlt", 36, [&mgr](Marshaler& m) {
        auto hdcDest = static_cast<Handle>(m.arg(0));
        if (hdcDest == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto x       = static_cast<i32>(m.arg(1));
        auto y       = static_cast<i32>(m.arg(2));
        auto cx      = static_cast<i32>(m.arg(3));
        auto cy      = static_cast<i32>(m.arg(4));
        auto hdcSrc  = static_cast<Handle>(m.arg(5));
        auto x1      = static_cast<i32>(m.arg(6));
        auto y1      = static_cast<i32>(m.arg(7));
        auto rop     = static_cast<u32>(m.arg(8));
        if (std::getenv("VITREO_GDI_DIAG") && m.has_cpu() &&
            !m.cpu_is_64bit()) {
            // cards.dll!cdtDrawExt retains its frame across the imported
            // BitBlt call. These values distinguish an upstream card/mode
            // selection error from a GDI bitmap-identity error.
            const auto frame = static_cast<GuestAddr>(
                static_cast<u32>(m.cpu()->gpr.rbp));
            const auto caller = m.read_u32(frame + 4);
            const auto card = m.read_u32(frame + 0x1c);
            const auto mode = m.read_u32(frame + 0x20);
            const auto record_ptr = m.read_u32(frame + 0x30);
            u16 record_word = 0xffffu;
            if (record_ptr) {
                record_word = m.read_u16(
                    static_cast<GuestAddr>(record_ptr.value()))
                                  .value_or(0xffffu);
            }
            VITREO_TRACE("[gdi-diag] BitBlt guest-frame ebp=0x%llx "
                         "caller=0x%x card=0x%x mode=0x%x "
                         "record=0x%x word=0x%x\n",
                         static_cast<unsigned long long>(frame),
                         caller.value_or(0xffffffffu),
                         card.value_or(0xffffffffu),
                         mode.value_or(0xffffffffu),
                         record_ptr.value_or(0xffffffffu),
                         record_word);
        }
        // Debug: log raw thunk parameters
        VITREO_TRACE( "[gdi] BitBlt raw: dest=0x%x x=%d y=%d cx=%d cy=%d src=0x%x x1=%d y1=%d rop=0x%x\n",
                     hdcDest, x, y, cx, cy, hdcSrc, x1, y1, rop);
        static const bool dump_dc = std::getenv("VITREO_DUMP_WINDOW_FB") != nullptr;
        if (dump_dc) {
            mgr.debug_report_dc_alpha(hdcSrc);
            mgr.debug_report_dc_alpha(hdcDest);
        }
        auto ok = mgr.bit_blt(hdcDest, x, y, cx, cy, hdcSrc, x1, y1, rop);
        m.set_return(ok ? 1u : 0u);
    }, 36});

    // ---- CreateSolidBrush(color) ----
    exports.entries.push_back({"CreateSolidBrush", 37, [&mgr](Marshaler& m) {
        auto color = static_cast<u32>(m.arg(0));
        auto h = mgr.create_solid_brush(color);
        m.set_return(static_cast<u64>(h));
    }, 4});

    // ---- PatBlt(hdc, x, y, w, h, rop) ----
    exports.entries.push_back({"PatBlt", 38, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        if (hdc == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto x   = static_cast<i32>(m.arg(1));
        auto y   = static_cast<i32>(m.arg(2));
        auto w   = static_cast<i32>(m.arg(3));
        auto h   = static_cast<i32>(m.arg(4));
        auto rop = static_cast<u32>(m.arg(5));
        auto ok = mgr.pat_blt(hdc, x, y, w, h, rop);
        m.set_return(ok ? 1u : 0u);
    }, 24});

    // ---- SetBrushOrgEx(hdc, x, y, lppt) ----
    exports.entries.push_back({"SetBrushOrgEx", 39, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto x   = static_cast<i32>(m.arg(1));
        auto y   = static_cast<i32>(m.arg(2));
        // lppt (arg3) — old origin, we don't store it
        auto ok = mgr.set_brush_org_ex(hdc, x, y);
        m.set_return(ok ? 1u : 0u);
    }, 16});

    // ---- GetTextMetricsW(hdc, lptm) ----
    exports.entries.push_back({"GetTextMetricsW", 40, [&mgr](Marshaler& m) { gdi_text_exports::get_text_metrics(mgr, m, true); }, 8});

    // Learn: https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getoutlinetextmetricsw
    exports.entries.push_back({"GetOutlineTextMetricsW", 0, [&mgr](Marshaler& m) {
        const auto hdc = static_cast<Handle>(m.arg(0));
        const auto cj = static_cast<u32>(m.arg(1));
        const GuestAddr potm = m.arg_ptr(2);
        m.set_return(mgr.get_outline_text_metrics(
            hdc, cj, potm, true, m.cpu_is_64bit()));
    }, 12});
    exports.entries.push_back({"GetOutlineTextMetricsA", 0, [&mgr](Marshaler& m) {
        const auto hdc = static_cast<Handle>(m.arg(0));
        const auto cj = static_cast<u32>(m.arg(1));
        const GuestAddr potm = m.arg_ptr(2);
        m.set_return(mgr.get_outline_text_metrics(
            hdc, cj, potm, false, m.cpu_is_64bit()));
    }, 12});

    // ---- LineDDA(xStart, yStart, xEnd, yEnd, lpProc, lData) ----
    // Stub — skip the per-pixel callback. Real Windows returns 0 when
    // lpProc is NULL (no last_error change).
    exports.entries.push_back({"LineDDA", 41, [](Marshaler& m) {
        if (m.arg(4) == 0) { m.set_return(0); return; }
        m.set_return(1u);
    }, 24});

    // ---- PtVisible(hdc, x, y) ----
    exports.entries.push_back({"PtVisible", 42, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto x   = static_cast<i32>(m.arg(1));
        auto y   = static_cast<i32>(m.arg(2));
        m.set_return(mgr.pt_visible(hdc, x, y) ? 1u : 0u);
    }, 12});

    // ---- GetPixel(hdc, x, y) ----
    exports.entries.push_back({"GetPixel", 43, [&mgr](Marshaler& m) {
        auto hdc = static_cast<Handle>(m.arg(0));
        auto x   = static_cast<i32>(m.arg(1));
        auto y   = static_cast<i32>(m.arg(2));
        m.set_return(static_cast<u64>(mgr.get_pixel(hdc, x, y)));
    }, 12});

    // ---- GetObjectA(hObj, cbBuffer, lpvObject) ----
    exports.entries.push_back({"GetObjectA", 44, [&mgr](Marshaler& m) {
        auto hObj   = static_cast<Handle>(m.arg(0));
        auto cb     = static_cast<i32>(m.arg32(1));
        auto lpObj  = m.arg_ptr(2);
        u32 error = 0;
        auto result = mgr.get_object(hObj, cb, lpObj, m.cpu_is_64bit(), false, &error);
        if (result == 0 && error != 0) set_last_error(error);
        m.set_return(static_cast<u64>(static_cast<u32>(result)));
    }, 12});

    // ---- StretchBlt(hdcDest, xd, yd, wd, hd, hdcSrc, xs, ys, ws, hs, rop) ----
    exports.entries.push_back({"StretchBlt", 45, [&mgr](Marshaler& m) {
        auto hdcDest = static_cast<Handle>(m.arg(0));
        if (hdcDest == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        auto xd      = static_cast<i32>(m.arg(1));
        auto yd      = static_cast<i32>(m.arg(2));
        auto wd      = static_cast<i32>(m.arg(3));
        auto hd      = static_cast<i32>(m.arg(4));
        auto hdcSrc  = static_cast<Handle>(m.arg(5));
        auto xs      = static_cast<i32>(m.arg(6));
        auto ys      = static_cast<i32>(m.arg(7));
        auto ws      = static_cast<i32>(m.arg(8));
        auto hs      = static_cast<i32>(m.arg(9));
        auto rop     = static_cast<u32>(m.arg(10));
        auto ok = mgr.stretch_blt(hdcDest, xd, yd, wd, hd, hdcSrc, xs, ys, ws, hs, rop);
        m.set_return(ok ? 1u : 0u);
    }, 44});
}

}  // namespace vitreo::winapi
