#pragma once

/// @file theme_render.hpp
/// @brief The macOS artwork behind uxtheme's DrawThemeBackground.
///
/// Every part is drawn with CoreGraphics in AppKit's semantic colours
/// (windowing::ThemePalette) at the rectangle and sizes Windows' Aero theme
/// uses: only the pixels differ from Windows, never the geometry an app lays
/// out against. No Windows theme art is used.

#include <vitreo/common.hpp>

#include "windowing/contracts/theme_host.hpp"
#include "winapi/ui/uxtheme/theme_schema.hpp"

#include <functional>
#include <vector>

namespace vitreo::winapi {
class GdiManager;
}

namespace vitreo::winapi::uxtheme {

/// What a part draw may leave out (DTBGOPTS dwFlags).
struct PartDrawOptions {
    bool omit_border = false;   ///< DTBG_OMITBORDER
    bool omit_content = false;  ///< DTBG_OMITCONTENT
    bool mirror = false;        ///< DTBG_MIRRORDC (or a mirrored DC without DTBG_NOMIRROR)
    /// The part is opaque (IsThemeBackgroundPartiallyTransparent FALSE):
    /// every pixel of its rectangle is painted, as Aero's image does.
    bool opaque = false;
};

/// A straight 32-bit canvas the renderer draws a part into: premultiplied
/// BGRA ("kCGImageAlphaPremultipliedFirst | ByteOrder32Little"), top row
/// first, transparent where the part has no pixels.
struct PartCanvas {
    i32 width = 0;
    i32 height = 0;
    std::vector<u8> bgra;
};

/// Draw @p cls / @p part / @p state at @p width x @p height device pixels.
/// @p scale is device pixels per 96-DPI pixel. False when the part has no
/// artwork (nothing to draw); the canvas is then transparent.
bool render_part(ThemeClass cls, i32 part, i32 state, i32 width, i32 height, double scale,
                 const windowing::ThemePalette& palette, bool dark, const PartDrawOptions& options,
                 PartCanvas& out);

/// Draw on @p hdc: @p render is called (under the DC's surface lock) with
/// the device-pixel size of the logical rectangle @p rect and the device
/// pixels per logical pixel; its canvas is composited source-over at the
/// rectangle, inside the DC's clip and @p clip (logical, may be null).
/// False for an unknown DC or one without a surface.
bool paint_on_dc(GdiManager& gdi, u64 hdc, const i32 rect[4], const i32* clip,
                 const std::function<bool(i32 width, i32 height, double scale, PartCanvas& out)>& render);

/// The edge DrawThemeEdge draws (BDR_* rings on the BF_* sides).
bool render_edge(i32 width, i32 height, double scale, u32 edge, u32 flags,
                 const windowing::ThemePalette& palette, bool dark, PartCanvas& out);

/// The built-in macOS palette (the documented AppKit system-colour values),
/// used when the host supplies none.
[[nodiscard]] windowing::ThemePalette default_palette(windowing::ThemeAppearance appearance);

}  // namespace vitreo::winapi::uxtheme
