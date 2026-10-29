// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_text_font_exports.cpp
/// @brief Text, glyph, font-resource and gamma-ramp exports.
///
/// GetCharABCWidthsW .. UnrealizeObject: character widths, glyph outlines
/// and indices, font resources, ANSI variants, and the device gamma ramp.
///
/// One of the section registrars split out of make_gdi32_exports
/// (gdi32_registration.cpp); see gdi32_registration_internal.hpp. The entries
/// are pushed in their original order.

#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/kernel32/error.hpp"

namespace vitreo::winapi {

void add_gdi32_text_font_exports(DllExports& exports, GdiManager& mgr) {
    // ------------------------------------------------------------------
    // ASCII variants of existing wide functions, plus a few font-resource
    // entry points.
    // ------------------------------------------------------------------
    exports.entries.push_back({"GetCharABCWidthsW", 0, [&mgr](Marshaler& m) { gdi_text_exports::get_char_abc_widths(mgr, m, true); }, 16});

    exports.entries.push_back({"GetCharABCWidthsA", 0, [&mgr](Marshaler& m) { gdi_text_exports::get_char_abc_widths(mgr, m, false); }, 16});

    // BOOL GetCharABCWidthsFloatW/A(HDC, UINT first, UINT last, LPABCFLOAT)
    exports.entries.push_back({"GetCharABCWidthsFloatW", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_char_abc_widths_float(mgr, m, true);
    }, 16});
    exports.entries.push_back({"GetCharABCWidthsFloatA", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_char_abc_widths_float(mgr, m, false);
    }, 16});

    // BOOL GetCharWidthW/A / GetCharWidth32W/A(HDC, UINT first, UINT last, LPINT)
    exports.entries.push_back({"GetCharWidthW", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_char_width(mgr, m, true);
    }, 16});
    exports.entries.push_back({"GetCharWidthA", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_char_width(mgr, m, false);
    }, 16});
    exports.entries.push_back({"GetCharWidth32W", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_char_width(mgr, m, true);
    }, 16});
    exports.entries.push_back({"GetCharWidth32A", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_char_width(mgr, m, false);
    }, 16});

    // BOOL GetCharABCWidthsI(HDC, UINT giFirst, UINT cgi, LPWORD pgi, LPABC)
    // Glyph-index ABC widths. qwindows imports this; same synthetic metrics
    // as GetCharABCWidthsW (abcA=0, abcB=8, abcC=0).
    exports.entries.push_back({"GetCharABCWidthsI", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 result = gdi_font_data::get_char_widths_i(mgr, m, true);
        // Success leaves the last error untouched (measured on Windows 11:
        // last_error_on_success_user_probe.expected).
        if (result == 0) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(result);
    }, 20});

    // BOOL GetCharWidthI(HDC, UINT giFirst, UINT cgi, LPWORD pgi, LPINT piWidths)
    exports.entries.push_back({"GetCharWidthI", 0, [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 result = gdi_font_data::get_char_widths_i(mgr, m, false);
        // Success leaves the last error untouched (measured on Windows 11:
        // last_error_on_success_user_probe.expected).
        if (result == 0) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(result);
    }, 20});

    // DWORD GetGlyphOutlineW(HDC, UINT uChar, UINT fuFormat,
    //     LPGLYPHMETRICS, DWORD cjBuffer, LPVOID pvBuffer, const MAT2*)
    // 7 args, stdcall_bytes=28. Qt qwindows calls this after ABC floats;
    // missing-export returned GDI_ERROR (0xFFFFFFFF) and the process
    // TerminateProcess'd itself with 0xc0000409.
    auto get_glyph_outline = [&mgr](Marshaler& m) {
        constexpr u32 GDI_ERROR = 0xFFFFFFFFu;
        if (m.arg(0) == 0) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(GDI_ERROR);
            return;
        }
        if (m.arg_ptr(3) == 0 || m.arg_ptr(6) == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(GDI_ERROR);
            return;
        }
        const u32 result = gdi_font_data::get_glyph_outline(mgr, m);
        set_last_error(result == GDI_ERROR
                           ? error_codes::ERROR_CALL_NOT_IMPLEMENTED
                           : error_codes::ERROR_SUCCESS);
        m.set_return(result);
    };
    exports.entries.push_back({"GetGlyphOutlineW", 0, get_glyph_outline, 28});
    exports.entries.push_back({"GetGlyphOutlineA", 0, get_glyph_outline, 28});

    // ---- GetGlyphIndicesW/A(hdc, lpstr, c, pgi, fl) ----
    exports.entries.push_back({"GetGlyphIndicesW", 0, [&mgr](Marshaler& m) {
        m.set_return(gdi_font_data::get_glyph_indices(mgr, m, true));
    }, 20});
    exports.entries.push_back({"GetGlyphIndicesA", 0, [&mgr](Marshaler& m) {
        m.set_return(gdi_font_data::get_glyph_indices(mgr, m, false));
    }, 20});

    exports.entries.push_back({"EnumFontFamiliesExA", 0, [&mgr](Marshaler& m) {
        // (hdc, lpLogfont, lpProc, lParam, dwFlags): font_enum.cpp. A NULL
        // LOGFONT enumerates every family ("NULL logfont ret=1").
        const GuestAddr lf = m.arg_ptr(1);
        GdiManager::FontInfo filter;
        filter.char_set = 1;  // DEFAULT_CHARSET
        if (lf != 0) filter = read_logfont(m, lf, false);
        m.set_return(static_cast<u32>(mgr.enum_font_families_ex(static_cast<Handle>(m.arg(0)), filter.face_name,
                                                                filter.char_set, m.arg_ptr(2), m.arg(3), false,
                                                                m.cpu_is_64bit())));
    }, 20});

    exports.entries.push_back({"SetDCBrushColor", 0, [&mgr](Marshaler& m) {
        m.set_return(static_cast<u64>(mgr.set_dc_brush_color(static_cast<Handle>(m.arg(0)),
                                                             m.arg32(1))));
    }, 8});

    exports.entries.push_back({"ExtTextOutA", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::ext_text_out(mgr, m, false);
    }, 32});

    exports.entries.push_back({"RemoveFontResourceA", 0, [&mgr](Marshaler& m) { gdi_text_exports::remove_font_resource(mgr, m, false, false); }, 4});

    exports.entries.push_back({"AddFontResourceExA", 0, [&mgr](Marshaler& m) { gdi_text_exports::add_font_resource(mgr, m, false, true); }, 12});

    exports.entries.push_back({"AddFontResourceExW", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::add_font_resource(mgr, m, true, true);
    }, 12});
    exports.entries.push_back({"AddFontResourceW", 0, [&mgr](Marshaler& m) { gdi_text_exports::add_font_resource(mgr, m, true, false); }, 4});
    exports.entries.push_back({"RemoveFontResourceExW", 0, [&mgr](Marshaler& m) { gdi_text_exports::remove_font_resource(mgr, m, true, true); }, 12});
    exports.entries.push_back({"RemoveFontResourceW", 0, [&mgr](Marshaler& m) { gdi_text_exports::remove_font_resource(mgr, m, true, false); }, 4});

    exports.entries.push_back({"AddFontResourceA", 0, [&mgr](Marshaler& m) { gdi_text_exports::add_font_resource(mgr, m, false, false); }, 4});

    // TEXTMETRICA is 53 bytes of data (56 with padding Windows leaves
    // alone). Callers size a stack buffer to that padded size.
    exports.entries.push_back({"GetTextMetricsA", 0, [&mgr](Marshaler& m) { gdi_text_exports::get_text_metrics(mgr, m, false); }, 8});

    // CreateFontA(nHeight, nWidth, nEscapement, nOrientation, fnWeight,
    //             fdwItalic, fdwUnderline, fdwStrikeOut, fdwCharSet,
    //             fdwOutputPrecision, fdwClipPrecision, fdwQuality,
    //             fdwPitchAndFamily, lpszFace)
    //
    // This previously discarded all 14 parameters and returned the constant
    // 0x0CAF0F71 as a pseudo-HFONT. That handle was never entered into the font
    // registry, so SelectObject did not recognise it as a font (it fell through
    // to the brush branch) and get_dc_font_info returned null — every font
    // measured identically no matter what was requested.
    //
    // A caller sizes its dialog font by creating candidate fonts and
    // measuring a sample string until the average character width fits, so a
    // measurement that ignores the font never converges and it spins.
    // Mirrors CreateFontW; only the face-name encoding differs and
    // the face name is not yet used for selection.
    exports.entries.push_back({"CreateFontA", 0, [&mgr](Marshaler& m) {
        m.set_return(static_cast<u64>(mgr.create_font(read_create_font_args(m, false))));
    }, 56});

    // ---- Gamma ramp ---------------------------------------------------------
    //
    // BOOL GetDeviceGammaRamp(HDC, LPVOID lpRamp)
    // BOOL SetDeviceGammaRamp(HDC, LPVOID lpRamp)
    //
    // The ramp is WORD[3][256] — 1536 bytes. Vitreo renders through Metal onto
    // a compositing window server, where a process cannot take over the
    // display's gamma table; there is no way to make SetDeviceGammaRamp's
    // visible effect happen. Games treat the failure as "no gamma control
    // available" and fall back to shader/software brightness, so reporting it
    // honestly is what keeps them correct. GetDeviceGammaRamp still fills in
    // the identity ramp it would read on a display with no adjustment applied,
    // which is a true description of the current state.
    exports.entries.push_back({"GetDeviceGammaRamp", 0, [&mgr](Marshaler& m) {
        // A memory DC has no display gamma: FALSE, error untouched
        // (s3gdi_print_probe "GetDeviceGammaRamp(mem)=0 err=57005").
        const auto hdc = static_cast<Handle>(m.arg(0));
        const GuestAddr ramp = m.arg_ptr(1);
        if (!mgr.has_dc(hdc) || mgr.object_type(hdc) == 10 /* OBJ_MEMDC */ || ramp == 0) {
            m.set_return(0u);
            return;
        }
        for (u32 channel = 0; channel < 3; ++channel) {
            for (u32 index = 0; index < 256; ++index) {
                // The identity ramp maps index i to i * 257, filling the
                // 16-bit range exactly as an unadjusted display would.
                const auto value = static_cast<u16>(index * 257);
                const GuestAddr slot =
                    ramp + (channel * 256 + index) * 2;
                if (m.write_u16(slot, value).is_err()) {
                    m.set_return(0u);
                    return;
                }
            }
        }
        m.set_return(1u);
    }, 8});

    exports.entries.push_back({"SetDeviceGammaRamp", 0, [](Marshaler& m) {
        (void)m.arg(0);
        (void)m.arg(1);
        VITREO_TRACE("[gdi32] SetDeviceGammaRamp -> FALSE (display gamma is "
                     "owned by the macOS window server)\n");
        m.set_return(0u);
    }, 8});

    // ---- BOOL UnrealizeObject(HGDIOBJ) --------------------------------------
    //
    // For a brush this resets the brush origin so the next selection realigns
    // the pattern; for a logical palette it forces a full remap on the next
    // RealizePalette. Vitreo draws brushes with an origin fixed at the device
    // origin and has no palette realization step, so the state this would
    // invalidate does not exist and the call has nothing left to do. A NULL
    // object is still rejected, as Windows does.
    exports.entries.push_back({"UnrealizeObject", 0, [](Marshaler& m) {
        const auto object = static_cast<u32>(m.arg(0));
        m.set_return(object != 0 ? 1u : 0u);
    }, 4});
}

}  // namespace vitreo::winapi
