// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#ifdef __OBJC__
@class NSImage;
#endif

namespace vitreo::windowing {

struct AppIconPresentationGeometry {
    double plate_x = 0.0;
    double plate_y = 0.0;
    double plate_size = 0.0;
    double corner_radius = 0.0;
    double artwork_x = 0.0;
    double artwork_y = 0.0;
    double artwork_size = 0.0;
};

/// Calculate the neutral macOS-style plate and guest-artwork placement.
[[nodiscard]] AppIconPresentationGeometry app_icon_presentation_geometry(
    double width, double height) noexcept;

#ifdef __OBJC__
/// Place guest icon artwork on a neutral macOS-style rounded-square plate.
///
/// This is a fallback for launches without a generated app bundle. The
/// returned image is autoreleased and the source image is not modified.
[[nodiscard]] NSImage* create_macos_presented_app_icon(NSImage* source_icon);
#endif

} // namespace vitreo::windowing
