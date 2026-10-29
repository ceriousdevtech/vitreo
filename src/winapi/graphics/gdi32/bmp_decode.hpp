#pragma once
/// @file bmp_decode.hpp
/// @brief BMP file (BITMAPFILEHEADER + DIB) to top-down RGBA8 decoding.
///
/// One decoder serves every consumer of a BMP image: the GDI bitmap host,
/// LoadImage, and the WIC BMP decoder. The result is always top-down RGBA
/// with opaque alpha; the source bit depth is reported so a caller that
/// must name the *native* pixel format (WIC's IWICBitmapFrameDecode) can.

#include <vitreo/common.hpp>

#include <optional>
#include <span>
#include <vector>

namespace vitreo::winapi {

struct DecodedBmp {
    i32 width{0};
    i32 height{0};
    u16 bits_per_pixel{0}; ///< 1, 4, 8, 24 or 32 as stored in the file
    /// 1 bpp only: the colour table is exactly black and white, in either
    /// order. Decides whether a compatible bitmap stays monochrome.
    bool black_white_table{false};
    std::vector<u8> rgba;  ///< width * height * 4, row-major, top-down
};

/// Decode a complete BMP file image. Returns nullopt for a file that is not
/// a BMP (bad magic or header), has an unsupported depth, or an absurd size.
[[nodiscard]] std::optional<DecodedBmp> decode_bmp_file(
    std::span<const u8> file);

/// Bits per pixel of the bitmap LoadBitmap / LoadImage (without
/// LR_CREATEDIBSECTION or LR_MONOCHROME) create from @p bmp: a bitmap
/// compatible with the display. A 1 bpp DIB stays monochrome only when its
/// colour table is black and white (either order); any other 1 bpp table,
/// {black,black} included, gives a display-depth colour bitmap holding the
/// table's colours (tools/oracle_probe/mono_dib_load_probe.expected, x86 and
/// x64 identical).
[[nodiscard]] u16 compatible_bitmap_bpp(const DecodedBmp& bmp,
                                        u16 display_bpp) noexcept;

} // namespace vitreo::winapi
