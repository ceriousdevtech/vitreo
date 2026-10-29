#pragma once

/// @file theme_data.hpp
/// @brief Windows 11 Aero's measured theme metrics (theme_aero_metrics.inc).
///
/// Apps lay their controls out with the numbers the theme reports: part
/// sizes, content and sizing margins, and the layout properties. Those come
/// verbatim from Aero (tools/oracle_probe/uxtheme_aero_probe.c, recorded in
/// tests/oracle_data/uxtheme_aero_probe{32,64}.expected); only the artwork
/// (theme_render.cpp) and the colours differ.

#include <vitreo/common.hpp>

#include "winapi/ui/uxtheme/theme_schema.hpp"

#include <span>
#include <string_view>

namespace vitreo::winapi::uxtheme {

/// GetThemePartSize(TS_DRAW) as Aero answers it.
enum class DrawKind : u8 {
    Invalid,      ///< E_INVALIDARG (border-fill and empty backgrounds)
    Stretch,      ///< the rectangle's size; 0x0 without one
    Zero,         ///< 0x0 whatever the rectangle
    TrueFixed,    ///< the true size whatever the rectangle
    TrueClamp,    ///< the true size, each side clamped to the rectangle
    TrueUniform,  ///< the true size, scaled down uniformly to fit the rectangle
};

/// One [part, state] section of a class.
struct PartStateMetrics {
    i16 part;
    i16 state;
    u8 flags;  ///< kDefined | kPartiallyTransparent | kNoRegion | kHasContentMargins | kHasSizingMargins
    DrawKind draw;
    i16 min_cx, min_cy;    ///< TS_MIN
    i16 true_cx, true_cy;  ///< TS_TRUE
    i16 content[4];        ///< content-rect insets: left, top, right, bottom
    i16 content_margins[4];  ///< TMT_CONTENTMARGINS: cxLeft, cxRight, cyTop, cyBottom
    i16 sizing_margins[4];   ///< TMT_SIZINGMARGINS
};

inline constexpr u8 kDefined = 1;
inline constexpr u8 kPartiallyTransparent = 2;
inline constexpr u8 kNoRegion = 4;  ///< GetThemeBackgroundRegion is E_UNEXPECTED
inline constexpr u8 kHasContentMargins = 8;
inline constexpr u8 kHasSizingMargins = 16;

enum class PropType : u8 { Enum, String, Int, Bool, Color, Margins, Filename, Size, Position, Rect, Font, IntList, Bitmap };

/// One property. part/state -1 mark where it is defined: state -1 = the
/// part's section (PO_PART), part -1 = the class section (PO_CLASS).
struct PropertyRecord {
    i16 id;
    i8 part;
    i8 state;
    PropType type;
    i32 value[4];  ///< int/bool/enum: [0]; margins/rect: [0..3]; position: [0..1]
    u16 extra;     ///< Font: index into kFonts; IntList: offset into kIntLists
};

struct ThemeFontRecord {
    i32 height, width, weight, italic, charset;
    std::u16string_view face;
};

/// TS_DRAW size of a DPI-scaled true-size image at a measured DPI.
struct DpiDrawSize {
    i16 variant, part;
    i16 dpi;
    i16 cx, cy;
};

struct VariantRecord {
    std::u16string_view subapp;  ///< "" for the base class, else e.g. "Explorer"
    std::u16string_view cls;     ///< VSCLASS_ name, upper case
    ThemeClass theme_class;
    u16 ps_begin, ps_count;
    u16 prop_begin, prop_count;
};

/// PROPERTYORIGIN.
enum class Origin : i32 { State = 0, Part = 1, Class = 2, Global = 3, NotFound = 4 };

/// A measured class (with its sub-application) by index into the variant table.
struct Variant {
    i32 index = -1;
    [[nodiscard]] bool valid() const { return index >= 0; }
    [[nodiscard]] const VariantRecord& record() const;
    [[nodiscard]] std::span<const PartStateMetrics> part_states() const;
    [[nodiscard]] std::span<const PropertyRecord> properties() const;
};

/// The section for [part, state], nullptr when the class has none.
[[nodiscard]] const PartStateMetrics* find_part_state(Variant v, i32 part, i32 state);

/// The property as Windows' lookup finds it: [part, state], then [part],
/// then the class section. @p origin receives where it was found.
[[nodiscard]] const PropertyRecord* find_property(Variant v, i32 part, i32 state, i32 id, Origin* origin);

/// Look up "subapp::class" (case-insensitive). With an unknown sub-app the
/// base class answers; nullopt-like invalid Variant for an unknown class.
[[nodiscard]] Variant find_variant(std::u16string_view subapp, std::u16string_view cls);

/// The measured TS_DRAW size of [part] at @p dpi, nullptr when not measured.
[[nodiscard]] const DpiDrawSize* find_dpi_draw_size(Variant v, i32 part, u32 dpi);

[[nodiscard]] const ThemeFontRecord& font_record(u16 index);
[[nodiscard]] std::span<const i32> int_list(u16 offset);

}  // namespace vitreo::winapi::uxtheme
