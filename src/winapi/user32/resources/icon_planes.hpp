#pragma once
/// @file icon_planes.hpp
/// @brief The image behind an HICON/HCURSOR: a colour plane and an AND mask.
///
/// Windows keeps an icon or cursor as the two bitmaps GetIconInfo hands back
/// (Learn: ICONINFO): hbmMask, a 1bpp AND mask, and hbmColor, the colour
/// (XOR) image - absent for a monochrome image, whose mask then holds the AND
/// rows followed by the XOR rows. Every icon export works from these planes:
/// CreateIconIndirect builds them, GetIconInfo copies them out, DrawIconEx
/// draws them, and the cursor presenter flattens them to RGBA.

#include <vitreo/common.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitreo::winapi {

struct IconPlanes {
    bool is_icon = true;
    u32 width = 0;
    u32 height = 0;     ///< visible height (half the mask for monochrome)
    u32 hotspot_x = 0;
    u32 hotspot_y = 0;
    /// width*height RGBA, top-down; the fourth byte is the colour bitmap's
    /// reserved/alpha byte. Empty for a monochrome image.
    std::vector<u8> color;
    /// One byte per pixel, 0 or 1, top-down: height rows, or 2*height rows
    /// (AND then XOR) for a monochrome image.
    std::vector<u8> mask;
    /// GetIconInfoEx identity of a resource-loaded image.
    u16 res_id = 0;
    std::u16string module_name;
    std::u16string res_name;

    [[nodiscard]] bool monochrome() const noexcept { return color.empty(); }
    /// A colour plane with any non-zero alpha byte is drawn with per-pixel
    /// alpha and its mask ignored (s3gdi_icon_probe "alpha+halfmask").
    [[nodiscard]] bool has_alpha() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
};

/// Decode one icon/cursor image resource: an RT_ICON/RT_CURSOR payload or a
/// CreateIconFromResourceEx buffer. A DIB image has a BITMAPINFOHEADER whose
/// height covers XOR and AND rows; a cursor's is preceded by the 4-byte
/// hotspot. A PNG image is decoded with the host codec. @p want_w/@p want_h
/// of 0 keep the image's size; otherwise the planes are scaled to it.
[[nodiscard]] std::optional<IconPlanes> decode_icon_image(std::span<const u8> data, bool is_icon,
                                                          u32 want_w, u32 want_h);

/// Decode the best image of an .ICO/.CUR file for @p want_w x @p want_h
/// (0 = the first image), scaled to that size.
[[nodiscard]] std::optional<IconPlanes> decode_icon_file(std::span<const u8> data, u32 want_w,
                                                         u32 want_h);

/// Scale both planes to @p w x @p h: colour averaged when shrinking and
/// bilinear when growing, mask nearest.
[[nodiscard]] IconPlanes scale_icon_planes(const IconPlanes& planes, u32 w, u32 h);

/// Planes for an RGBA-only image (a presenter cursor): colour carries the
/// alpha, mask set where alpha is 0.
[[nodiscard]] IconPlanes icon_planes_from_rgba(u32 width, u32 height, std::span<const u8> rgba);

/// The straight-alpha RGBA the cursor presenter shows. An alpha plane is used
/// as is; otherwise AND=0 is opaque colour and AND=1 is transparent (an
/// inverting pixel cannot be drawn by the host cursor and is left clear).
[[nodiscard]] std::vector<u8> icon_display_rgba(const IconPlanes& planes);

}  // namespace vitreo::winapi
