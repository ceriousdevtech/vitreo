#pragma once
/// @file gdi32_registration_internal.hpp
/// @brief Private surface between make_gdi32_exports and its section registrars.
///
/// make_gdi32_exports was one 2,500-line function that pushed every gdi32
/// export in turn. It is now an orchestrator that calls one registrar per
/// contiguous run of entries, in the order the entries were always pushed:
/// DllExports::entries order decides each thunk's address and the order
/// dump_thunks prints, both recorded by the export inventory, so the call order
/// in make_gdi32_exports must not change.
///
/// Each registrar takes exactly the objects its handlers captured from
/// make_gdi32_exports (mgr; the display-adapter-name provider for D3DKMT); the
/// captures themselves are unchanged. The registrars that read GdiManager
/// state directly are friends of GdiManager (gdi32.hpp). The helpers shared by
/// more than one registrar are `inline` here so each handler still inlines
/// them. Private to gdi32 -- not part of any public surface.

#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>

namespace vitreo::winapi {

// =============================================================================
// Section registrars, in the order make_gdi32_exports calls them
// =============================================================================

/// D3DKMTOpenAdapterFromGdiDisplayName .. D3DKMTEnumAdapters
/// (gdi32_d3dkmt_exports.cpp). Takes the display-adapter-name provider
/// by value: the write_adapter_identity lambda copies it, as it always did.
void add_gdi32_d3dkmt_exports(
    DllExports& exports,
    std::function<std::string()> display_adapter_name_provider);

/// GetDC .. AddFontMemResourceEx (gdi32_dc_object_exports.cpp).
void add_gdi32_dc_object_exports(DllExports& exports, GdiManager& mgr);

/// ChoosePixelFormat .. StretchBlt (gdi32_drawing_exports.cpp).
void add_gdi32_drawing_exports(DllExports& exports, GdiManager& mgr);

/// GetDCOrgEx .. StretchDIBits (gdi32_dc_state_palette_exports.cpp).
void add_gdi32_dc_state_palette_exports(DllExports& exports, GdiManager& mgr);

/// ExtTextOutW .. GetTextCharsetInfo (gdi32_viewport_region_exports.cpp).
/// Friend of GdiManager: reads m_dc_states and m_region_objects.
void add_gdi32_viewport_region_exports(DllExports& exports, GdiManager& mgr);

/// Rectangle .. GetWindowOrgEx (gdi32_shape_region_exports.cpp).
/// Friend of GdiManager: reads m_dc_states, m_pen_objects,
/// m_brush_colors and m_region_objects.
void add_gdi32_shape_region_exports(DllExports& exports, GdiManager& mgr);

/// GetTextFaceW .. GetBkColor (gdi32_dc_attribute_exports.cpp).
/// Friend of GdiManager: reads m_dc_states and m_region_objects.
void add_gdi32_dc_attribute_exports(DllExports& exports, GdiManager& mgr);

/// CopyMetaFileW .. GetStretchBltMode
/// (gdi32_metafile_text_query_exports.cpp).
void add_gdi32_metafile_text_query_exports(DllExports& exports, GdiManager& mgr);

/// GetCharABCWidthsW .. UnrealizeObject (gdi32_text_font_exports.cpp).
void add_gdi32_text_font_exports(DllExports& exports, GdiManager& mgr);

/// BeginPath .. SetAbortProc (gdi32_path_print_exports.cpp).
/// Friend of GdiManager: reads m_region_objects.
void add_gdi32_path_print_exports(DllExports& exports, GdiManager& mgr);

// =============================================================================
// Helpers shared by more than one registrar
// =============================================================================

/// Reads LOGFONTW::lfFaceName, which is a fixed 32-WCHAR array that is only
/// null-terminated when the name is shorter than the array.
/// LOGFONTW / LOGFONTA, every field (Learn: LOGFONTW). The face name of
/// LOGFONTA is converted from the ANSI code page's ASCII range.
inline GdiManager::FontInfo read_logfont(Marshaler& m, GuestAddr lf, bool wide) {
    GdiManager::FontInfo f;
    u8 raw[92] = {};
    (void)m.memory().read_bytes(lf, raw, wide ? 92 : 60);
    auto le32 = [&](usize o) {
        return static_cast<i32>(static_cast<u32>(raw[o]) | (static_cast<u32>(raw[o + 1]) << 8) |
                                (static_cast<u32>(raw[o + 2]) << 16) |
                                (static_cast<u32>(raw[o + 3]) << 24));
    };
    f.height = le32(0);
    f.width = le32(4);
    f.escapement = le32(8);
    f.orientation = le32(12);
    f.lf_weight = le32(16);
    f.weight = static_cast<u16>(std::clamp(f.lf_weight, 0, 1000));
    f.italic = raw[20] != 0;
    f.underline = raw[21];
    f.strike_out = raw[22];
    f.char_set = raw[23];
    f.out_precision = raw[24];
    f.clip_precision = raw[25];
    f.quality = raw[26];
    f.pitch_and_family = raw[27];
    for (usize i = 0; i < 32; ++i) {
        const u32 unit = wide ? (static_cast<u32>(raw[28 + i * 2]) | (static_cast<u32>(raw[29 + i * 2]) << 8))
                              : raw[28 + i];
        if (unit == 0) break;
        f.face_name.push_back(static_cast<char16_t>(unit));
    }
    return f;
}

/// CreateFontW / CreateFontA: the fourteen LOGFONT fields as arguments.
/// Arguments past the fourth are DWORDs on the x64 stack.
inline GdiManager::FontInfo read_create_font_args(Marshaler& m, bool wide) {
    GdiManager::FontInfo f;
    f.height = static_cast<i32>(m.arg32(0));
    f.width = static_cast<i32>(m.arg32(1));
    f.escapement = static_cast<i32>(m.arg32(2));
    f.orientation = static_cast<i32>(m.arg32(3));
    f.lf_weight = static_cast<i32>(m.arg32(4));
    f.weight = static_cast<u16>(std::clamp(f.lf_weight, 0, 1000));
    f.italic = (m.arg32(5) & 0xFF) != 0;
    f.underline = static_cast<u8>(m.arg32(6));
    f.strike_out = static_cast<u8>(m.arg32(7));
    f.char_set = static_cast<u8>(m.arg32(8));
    f.out_precision = static_cast<u8>(m.arg32(9));
    f.clip_precision = static_cast<u8>(m.arg32(10));
    f.quality = static_cast<u8>(m.arg32(11));
    f.pitch_and_family = static_cast<u8>(m.arg32(12));
    const GuestAddr face = m.arg_ptr(13);
    for (u32 i = 0; face != 0 && i < 32; ++i) {
        u32 unit = 0;
        if (wide) {
            const auto r = m.read_u16(face + i * 2);
            if (r.is_err()) break;
            unit = r.value();
        } else {
            const auto r = m.read_u8(face + i);
            if (r.is_err()) break;
            unit = r.value();
        }
        if (unit == 0) break;
        f.face_name.push_back(static_cast<char16_t>(unit));
    }
    return f;
}

/// CreateDC / CreateIC: 1 for the display driver, 0 for no driver at all
/// (NULL, error untouched), -1 for any other driver (a printer Vitreo does
/// not have). s3gdi_print_probe: "CreateDCW(display)=hdc",
/// "CreateDCW(NULL)=NULL err=57005", "CreateDCW(NoSuchPrinter)=NULL err=1801".
inline int gdi_display_device(Marshaler& m, GuestAddr driver, bool wide) {
    if (driver == 0) return 0;
    auto r = wide ? m.read_wide_string(driver, 64) : m.read_ansi_string(driver, 64);
    std::string v = r.is_ok() ? r.value() : std::string{};
    if (v.empty()) return 0;
    for (char& c : v) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return v == "DISPLAY" ? 1 : -1;
}

}  // namespace vitreo::winapi
