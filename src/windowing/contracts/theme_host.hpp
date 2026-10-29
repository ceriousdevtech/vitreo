#pragma once

/// @file theme_host.hpp
/// @brief Host appearance for uxtheme's visual-style renderer.
///
/// uxtheme keeps Windows' theme METRICS (part sizes, margins, ints) and draws
/// the parts with macOS artwork. The colours come from AppKit's semantic
/// colours, resolved by the host on its main thread and handed over as plain
/// values: the draw path runs on arbitrary guest threads and never touches
/// AppKit itself.

#include <vitreo/common.hpp>

#include <array>

namespace vitreo::windowing {

/// Which AppKit appearance a palette was resolved under.
enum class ThemeAppearance : u8 { Light = 0, Dark = 1 };

/// The AppKit semantic colours the renderer draws with.
enum class ThemeColor : u8 {
    WindowBackground,       ///< windowBackgroundColor
    UnderPageBackground,    ///< underPageBackgroundColor
    ControlBackground,      ///< controlBackgroundColor (list/table background)
    Control,                ///< controlColor (button face)
    ControlText,            ///< controlTextColor
    DisabledControlText,    ///< disabledControlTextColor
    Label,                  ///< labelColor
    SecondaryLabel,         ///< secondaryLabelColor
    TertiaryLabel,          ///< tertiaryLabelColor
    QuaternaryLabel,        ///< quaternaryLabelColor
    Separator,              ///< separatorColor
    Grid,                   ///< gridColor
    TextBackground,         ///< textBackgroundColor
    Text,                   ///< textColor
    SelectedTextBackground, ///< selectedTextBackgroundColor
    Accent,                 ///< controlAccentColor
    SelectedContentBackground,             ///< selectedContentBackgroundColor
    UnemphasizedSelectedContentBackground, ///< unemphasizedSelectedContentBackgroundColor
    AlternateSelectedControlText,          ///< alternateSelectedControlTextColor
    KeyboardFocusIndicator, ///< keyboardFocusIndicatorColor
    Link,                   ///< linkColor
    HeaderText,             ///< headerTextColor
    ShadowColor,            ///< shadowColor
    SystemRed,              ///< systemRedColor
    SystemYellow,           ///< systemYellowColor
    SystemGreen,            ///< systemGreenColor
    SystemGray,             ///< systemGrayColor
    Count
};

inline constexpr usize kThemeColorCount = static_cast<usize>(ThemeColor::Count);

/// Straight-alpha sRGB colours, 0xAARRGGBB, indexed by ThemeColor.
struct ThemePalette {
    std::array<u32, kThemeColorCount> argb{};
    [[nodiscard]] u32 operator[](ThemeColor c) const noexcept {
        return argb[static_cast<usize>(c)];
    }
};

class ThemeHost {
public:
    virtual ~ThemeHost() = default;
    /// AppKit's semantic colours under @p appearance. False when the host has
    /// no AppKit (the renderer then uses its built-in macOS defaults).
    [[nodiscard]] virtual bool theme_palette(ThemeAppearance appearance,
                                             ThemePalette& out) {
        (void)appearance; (void)out;
        return false;
    }
};

} // namespace vitreo::windowing
