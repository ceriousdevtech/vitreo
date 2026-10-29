#pragma once

/// @file theme_schema.hpp
/// @brief Visual-style class, part, state and property ids (SDK vsstyle.h /
///        vssym32.h, generated into theme_schema.inc).

#include <vitreo/common.hpp>

#include <array>
#include <string_view>

namespace vitreo::winapi::uxtheme {

#include "winapi/ui/uxtheme/theme_schema.inc"

/// The visual-style classes uxtheme draws, in theme_schema.inc order.
enum class ThemeClass : u8 {
#define VITREO_UXTHEME_CLASS(id, name, maxpart, maxstate) id,
#define VITREO_UXTHEME_CLASS_TABLE
#include "winapi/ui/uxtheme/theme_schema.inc"
#undef VITREO_UXTHEME_CLASS_TABLE
#undef VITREO_UXTHEME_CLASS
    Count
};

inline constexpr usize kThemeClassCount = static_cast<usize>(ThemeClass::Count);

struct ThemeClassInfo {
    std::u16string_view name;  ///< VSCLASS_ name, upper case
    i32 max_part;              ///< largest *PARTS value
    i32 max_state;             ///< largest value of any of the class's *STATES enums
};

inline constexpr std::array<ThemeClassInfo, kThemeClassCount> kThemeClasses = {{
#define VITREO_UXTHEME_CLASS(id, name, maxpart, maxstate) {name, maxpart, maxstate},
#define VITREO_UXTHEME_CLASS_TABLE
#include "winapi/ui/uxtheme/theme_schema.inc"
#undef VITREO_UXTHEME_CLASS_TABLE
#undef VITREO_UXTHEME_CLASS
}};

[[nodiscard]] inline const ThemeClassInfo& class_info(ThemeClass c) {
    return kThemeClasses[static_cast<usize>(c)];
}

}  // namespace vitreo::winapi::uxtheme
