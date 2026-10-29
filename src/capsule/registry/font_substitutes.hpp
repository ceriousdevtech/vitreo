#pragma once
/// @file font_substitutes.hpp
/// @brief Vitreo's logical font aliases: HKLM\...\FontSubstitutes and GDI's
///        face-name mapper read the same set.
///
/// Only aliases whose meaning Microsoft documents are listed, each with its
/// source. This is Vitreo's own alias set, not a copy of a Windows registry
/// export.
///
///  - "MS Shell Dlg" and "MS Shell Dlg 2": Learn, "Using MS Shell Dlg and MS
///    Shell Dlg 2"
///    (https://learn.microsoft.com/windows/win32/intl/using-ms-shell-dlg-and-ms-shell-dlg-2),
///    section "Map the Logical Fonts": the mappings live in
///    HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes; on
///    Windows 2000 and later "MS Shell Dlg" maps to Microsoft Sans Serif
///    unless the machine default UI language is Japanese (then MS UI
///    Gothic), and "MS Shell Dlg 2 simply uses the Tahoma font regardless of
///    language". Vitreo's machine UI language is en-US (0x0409,
///    GetSystemDefaultUILanguage), so the non-Japanese mapping applies.
///
/// A target is resolved through the normal host font matching (gdi32
/// GdiManager::match_face, dwrite::match_system_font); when the host has no
/// such family the request falls through to the default face like any other
/// unknown name. Names not listed here are not aliases.

#include <array>
#include <string_view>

namespace vitreo::capsule {

struct FontSubstitute {
    std::u16string_view alias;
    std::u16string_view target;
};

inline constexpr std::array<FontSubstitute, 2> kFontSubstitutes = {{
    {u"MS Shell Dlg", u"Microsoft Sans Serif"},
    {u"MS Shell Dlg 2", u"Tahoma"},
}};

}  // namespace vitreo::capsule
