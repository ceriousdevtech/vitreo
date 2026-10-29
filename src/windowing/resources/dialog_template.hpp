#pragma once
/// @file dialog_template.hpp
/// @brief DLGTEMPLATE / DLGTEMPLATEEX parser for Win32 dialog resources

#include <vitreo/common.hpp>
#include <algorithm>
#include <string>
#include <vector>

namespace vitreo::windowing {

/// Standard Win32 control class ordinals
namespace ControlClass {
    constexpr u16 BUTTON   = 0x0080;
    constexpr u16 EDIT     = 0x0081;
    constexpr u16 STATIC   = 0x0082;
    constexpr u16 LISTBOX  = 0x0083;
    constexpr u16 SCROLLBAR = 0x0084;
    constexpr u16 COMBOBOX = 0x0085;
} // namespace ControlClass

/// Button sub-styles (BS_*)
namespace BS {
    constexpr u32 PUSHBUTTON    = 0x00000000;
    constexpr u32 DEFPUSHBUTTON = 0x00000001;
    constexpr u32 CHECKBOX      = 0x00000002;
    constexpr u32 AUTOCHECKBOX  = 0x00000003;
    constexpr u32 RADIOBUTTON   = 0x00000004;
    constexpr u32 GROUPBOX      = 0x00000007;
    constexpr u32 AUTORADIO     = 0x00000009;
    constexpr u32 TYPE_MASK     = 0x0000000F;
} // namespace BS

/// Static sub-styles (SS_*)
namespace SS {
    constexpr u32 LEFT          = 0x00000000;
    constexpr u32 CENTER        = 0x00000001;
    constexpr u32 RIGHT         = 0x00000002;
    constexpr u32 ICON          = 0x00000003;
    constexpr u32 SIMPLE        = 0x0000000B;
    constexpr u32 ETCHEDHORZ    = 0x00000010;
    constexpr u32 ETCHEDVERT    = 0x00000011;
    constexpr u32 ETCHEDFRAME   = 0x00000012;
    constexpr u32 TYPE_MASK     = 0x0000001F;
} // namespace SS

/// Edit sub-styles (ES_*)
namespace ES {
    constexpr u32 LEFT          = 0x00000000;
    constexpr u32 CENTER        = 0x00000001;
    constexpr u32 RIGHT         = 0x00000002;
    constexpr u32 MULTILINE     = 0x00000004;
    constexpr u32 PASSWORD      = 0x00000020;
    constexpr u32 AUTOHSCROLL   = 0x00000080;
    constexpr u32 AUTOVSCROLL   = 0x00000040;
    constexpr u32 READONLY      = 0x00000800;
} // namespace ES

/// ComboBox sub-styles (CBS_*)
namespace CBS {
    constexpr u32 SIMPLE        = 0x00000001;
    constexpr u32 DROPDOWN      = 0x00000002;
    constexpr u32 DROPDOWNLIST  = 0x00000003;
    constexpr u32 TYPE_MASK     = 0x00000003;
} // namespace CBS

/// DS_* dialog styles
namespace DS {
    constexpr u32 ABSALIGN      = 0x00000001;
    constexpr u32 SETFONT       = 0x00000040;
    constexpr u32 MODALFRAME    = 0x00000080;
    constexpr u32 CENTER        = 0x00000800;
    constexpr u32 CENTERMOUSE   = 0x00001000;
    constexpr u32 SHELLFONT     = 0x00000040;  // same bit as SETFONT
    constexpr u32 SETFOREGROUND = 0x00000200;
    constexpr u32 CONTROL       = 0x00000400;
} // namespace DS

/// Parsed dialog control item
struct DialogItem {
    u32 style     = 0;
    u32 ex_style  = 0;
    i16 x         = 0;
    i16 y         = 0;
    i16 cx        = 0;
    i16 cy        = 0;
    u32 id        = 0;

    /// Class: ordinal (0x0080-0x0085) or string
    u16 class_ordinal  = 0;         ///< Non-zero if standard class
    std::wstring class_name;        ///< Non-empty if custom class

    /// Control text / title
    std::wstring title;
    u16 title_ordinal = 0;  ///< Non-zero if title is a resource ordinal (e.g. icon ID)

    /// Extra creation data size in bytes
    u16 extra_count = 0;
};

/// Parsed dialog template
struct DialogTemplate {
    bool is_extended   = false;     ///< DLGTEMPLATEEX vs DLGTEMPLATE
    u32  help_id       = 0;         ///< Only for extended
    u32  style         = 0;
    u32  ex_style      = 0;
    u16  item_count    = 0;
    i16  x             = 0;
    i16  y             = 0;
    i16  cx            = 0;
    i16  cy            = 0;

    /// Menu: ordinal or string (empty = no menu)
    std::wstring menu;

    /// Window class: ordinal or string (empty = default #32770)
    std::wstring window_class;

    /// Dialog title
    std::wstring title;

    /// Font (present if DS_SETFONT | DS_SHELLFONT)
    u16 font_size      = 0;
    u16 font_weight    = 0;
    u8  font_italic    = 0;
    u8  font_charset   = 0;
    std::wstring font_face;

    /// Parsed control items
    std::vector<DialogItem> items;
};

/// Parse a dialog template from raw resource bytes.
/// @param data Pointer to the start of the DLGTEMPLATE/DLGTEMPLATEEX data
/// @param size Size of the data in bytes
/// @return Parsed template, or empty optional on parse failure
[[nodiscard]] VResult<DialogTemplate> parse_dialog_template(
    const u8* data, usize size);

/// Convert dialog units to pixels.
/// @param dlu Dialog unit value
/// @param base_unit Base unit (horizontal or vertical)
/// Default base units (6/13) match 8pt MS Shell Dlg / Tahoma at 96 DPI.
/// (tmAveCharWidth=6, tmHeight=13 for Tahoma 8pt.)
/// @return Pixel value
[[nodiscard]] inline i32 dlu_to_px_x(i16 dlu, i32 base_x = 6) noexcept {
    return (dlu * base_x) / 4;
}

[[nodiscard]] inline i32 dlu_to_px_y(i16 dlu, i32 base_y = 13) noexcept {
    return (dlu * base_y) / 8;
}

/// Compute DLU base units from a dialog template font size (in points).
/// Uses the standard 8pt->(6,13) baseline and scales proportionally.
inline void compute_dlu_bases(u16 font_pt, i32& base_x, i32& base_y) noexcept {
    if (font_pt == 0) font_pt = 8;
    base_x = std::max(4, static_cast<i32>((font_pt * 6 + 4) / 8));
    base_y = std::max(8, static_cast<i32>((font_pt * 13 + 4) / 8));
}

} // namespace vitreo::windowing
