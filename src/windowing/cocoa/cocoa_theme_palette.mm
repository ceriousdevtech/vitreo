/// @file cocoa_theme_palette.mm
/// @brief ThemeHost: AppKit's semantic colours for uxtheme's renderer.
///
/// The colours are resolved on the main thread under the named appearance
/// and cached as plain sRGB values. uxtheme draws on guest threads and only
/// reads the cache; a system colour change (accent, highlight, appearance
/// settings) clears it so the next draw picks the new colours up.

#import <AppKit/AppKit.h>

#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/cocoa/objc_helpers.h"

#include <array>
#include <cmath>
#include <mutex>
#include <optional>

namespace vitreo::windowing {

namespace {

struct PaletteCache {
    std::mutex mutex;
    std::array<std::optional<ThemePalette>, 2> palettes;
    bool observing = false;
};

PaletteCache& cache() {
    static PaletteCache c;
    return c;
}

u32 to_argb(NSColor* colour) {
    NSColor* srgb = [colour colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (srgb == nil) return 0;
    auto byte = [](CGFloat v) { return static_cast<u32>(std::lround(std::clamp<CGFloat>(v, 0, 1) * 255)); };
    return (byte([srgb alphaComponent]) << 24) | (byte([srgb redComponent]) << 16) |
           (byte([srgb greenComponent]) << 8) | byte([srgb blueComponent]);
}

ThemePalette resolve(NSAppearanceName name) {
    __block ThemePalette p;
    ThemePalette* out = &p;
    NSAppearance* appearance = [NSAppearance appearanceNamed:name];
    [appearance performAsCurrentDrawingAppearance:^{
        auto set = [out](ThemeColor k, NSColor* c) { out->argb[static_cast<usize>(k)] = to_argb(c); };
        set(ThemeColor::WindowBackground, [NSColor windowBackgroundColor]);
        set(ThemeColor::UnderPageBackground, [NSColor underPageBackgroundColor]);
        set(ThemeColor::ControlBackground, [NSColor controlBackgroundColor]);
        set(ThemeColor::Control, [NSColor controlColor]);
        set(ThemeColor::ControlText, [NSColor controlTextColor]);
        set(ThemeColor::DisabledControlText, [NSColor disabledControlTextColor]);
        set(ThemeColor::Label, [NSColor labelColor]);
        set(ThemeColor::SecondaryLabel, [NSColor secondaryLabelColor]);
        set(ThemeColor::TertiaryLabel, [NSColor tertiaryLabelColor]);
        set(ThemeColor::QuaternaryLabel, [NSColor quaternaryLabelColor]);
        set(ThemeColor::Separator, [NSColor separatorColor]);
        set(ThemeColor::Grid, [NSColor gridColor]);
        set(ThemeColor::TextBackground, [NSColor textBackgroundColor]);
        set(ThemeColor::Text, [NSColor textColor]);
        set(ThemeColor::SelectedTextBackground, [NSColor selectedTextBackgroundColor]);
        set(ThemeColor::Accent, [NSColor controlAccentColor]);
        set(ThemeColor::SelectedContentBackground, [NSColor selectedContentBackgroundColor]);
        set(ThemeColor::UnemphasizedSelectedContentBackground,
            [NSColor unemphasizedSelectedContentBackgroundColor]);
        set(ThemeColor::AlternateSelectedControlText, [NSColor alternateSelectedControlTextColor]);
        set(ThemeColor::KeyboardFocusIndicator, [NSColor keyboardFocusIndicatorColor]);
        set(ThemeColor::Link, [NSColor linkColor]);
        set(ThemeColor::HeaderText, [NSColor headerTextColor]);
        set(ThemeColor::ShadowColor, [NSColor shadowColor]);
        set(ThemeColor::SystemRed, [NSColor systemRedColor]);
        set(ThemeColor::SystemYellow, [NSColor systemYellowColor]);
        set(ThemeColor::SystemGreen, [NSColor systemGreenColor]);
        set(ThemeColor::SystemGray, [NSColor systemGrayColor]);
    }];
    return p;
}

}  // namespace

bool CocoaBridge::theme_palette(ThemeAppearance appearance, ThemePalette& out) {
    const usize index = appearance == ThemeAppearance::Dark ? 1 : 0;
    auto& c = cache();
    {
        std::lock_guard lock(c.mutex);
        if (c.palettes[index]) {
            out = *c.palettes[index];
            return true;
        }
    }
    ensure_app_initialized();
    __block ThemePalette resolved;
    vtr_run_on_main_sync(^{
        resolved = resolve(index == 1 ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua);
        std::lock_guard lock(c.mutex);
        if (!c.observing) {
            c.observing = true;
            [[NSNotificationCenter defaultCenter]
                addObserverForName:NSSystemColorsDidChangeNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification*) {
                            std::lock_guard inner(cache().mutex);
                            cache().palettes = {};
                        }];
        }
    });
    std::lock_guard lock(c.mutex);
    c.palettes[index] = resolved;
    out = resolved;
    return true;
}

}  // namespace vitreo::windowing
