#pragma once
/// @file default_palettes.hpp
/// @brief The colour tables GDI synthesises for indexed DIB formats, and the
///        default (static) palette, built from their documented definitions.
///
/// Learn, GetDIBits remarks
/// (https://learn.microsoft.com/windows/win32/api/wingdi/nf-wingdi-getdibits):
/// a 1bpp table is black and white; a 4bpp table is "identical to the
/// standard VGA palette"; an 8bpp table is "a general mix of 256 colors
/// defined by GDI", including "the 20 colors found in the default logical
/// palette". Learn, "Default Palette"
/// (https://learn.microsoft.com/windows/win32/gdi/default-palette): the
/// default palette typically has 20 entries, "the 16 standard colors of the
/// VGA display and 4 other colors defined by Windows". Learn, "System
/// Palette" (https://learn.microsoft.com/windows/win32/gdi/system-palette):
/// the static system-palette entries correspond to the default palette.
///
/// Colour values:
///  - The 16 VGA colours are the sixteen colour names of HTML 3.2 ("The BODY
///    element": "the standard 16 colors supported with the Windows VGA
///    palette"), https://www.w3.org/TR/2018/SPSD-html32-20180315/. Their
///    index order in the 4bpp table is measured behaviour (which index means
///    which colour: tools/oracle_probe/s3gdi_info_probe "nearest4").
///  - Learn does not name the 4 extra default colours or the rest of the 8bpp
///    mix. Vitreo defines them (.docs/contracts/gdi32.md § GetDIBits colour
///    tables): the extras complete an even grey ramp 0x40..0xE0 in 0x20 steps
///    with VGA Gray (0x80) and Silver (0xC0); the 236 middle entries are the
///    6x6x6 colour cube (levels 0, 51, ..., 255) followed by 20 greys
///    round(255 * k / 21), k = 1..20.
///
/// Layout of the 8bpp table: the 20 static colours occupy the first and last
/// ten entries (GetSystemPaletteEntries reserves the same slots). The index
/// of each VGA colour is measured behaviour (s3gdi_info_probe "palette8
/// vga_index": Black..Teal at 0..6, Silver at 7, Gray at 248, Red..White at
/// 249..255, so black is 0 and white 255, the two entries SYSPAL_NOSTATIC
/// keeps); the remaining slots are Vitreo's:
///   0..6     VGA 0..6 (4bpp order)       7        Silver
///   8, 9     extras 0x404040, 0x606060   10..225  colour cube, r slowest
///   226..245 grey ramp                   246, 247 extras 0xA0A0A0, 0xE0E0E0
///   248      Gray                        249..255 VGA 9..15

#include <vitreo/common.hpp>

#include <array>

namespace vitreo::winapi::gdi {

namespace palette_detail {

// HTML 3.2 colour names (0x00RRGGBB).
inline constexpr u32 kBlack = 0x000000, kMaroon = 0x800000, kGreen = 0x008000, kOlive = 0x808000,
                     kNavy = 0x000080, kPurple = 0x800080, kTeal = 0x008080, kGray = 0x808080,
                     kSilver = 0xC0C0C0, kRed = 0xFF0000, kLime = 0x00FF00, kYellow = 0xFFFF00,
                     kBlue = 0x0000FF, kFuchsia = 0xFF00FF, kAqua = 0x00FFFF, kWhite = 0xFFFFFF;

constexpr u32 grey(u32 v) { return (v << 16) | (v << 8) | v; }

constexpr std::array<u32, 256> build_palette8(const std::array<u32, 16>& vga) {
    std::array<u32, 256> out{};
    for (u32 i = 0; i < 7; ++i) out[i] = vga[i];
    out[7] = vga[8];  // Silver
    out[8] = grey(0x40);
    out[9] = grey(0x60);
    u32 n = 10;
    for (u32 r = 0; r < 6; ++r)
        for (u32 g = 0; g < 6; ++g)
            for (u32 b = 0; b < 6; ++b) out[n++] = ((r * 51) << 16) | ((g * 51) << 8) | (b * 51);
    for (u32 k = 1; k <= 20; ++k) out[n++] = grey((255 * k * 2 + 21) / 42);  // round(255k/21)
    out[246] = grey(0xA0);
    out[247] = grey(0xE0);
    out[248] = vga[7];  // Gray
    for (u32 i = 9; i < 16; ++i) out[240 + i] = vga[i];
    return out;
}

}  // namespace palette_detail

/// GetDIBits colour table for a 4bpp request (0x00RRGGBB): the 16 VGA colours.
inline constexpr std::array<u32, 16> kGdiDefaultPalette4 = {
    palette_detail::kBlack, palette_detail::kMaroon, palette_detail::kGreen,  palette_detail::kOlive,
    palette_detail::kNavy,  palette_detail::kPurple, palette_detail::kTeal,   palette_detail::kGray,
    palette_detail::kSilver, palette_detail::kRed,   palette_detail::kLime,   palette_detail::kYellow,
    palette_detail::kBlue,  palette_detail::kFuchsia, palette_detail::kAqua,  palette_detail::kWhite,
};

/// GetDIBits colour table for an 8bpp request (0x00RRGGBB); also the system
/// palette GetSystemPaletteEntries reports.
inline constexpr std::array<u32, 256> kGdiDefaultPalette8 =
    palette_detail::build_palette8(kGdiDefaultPalette4);

/// DEFAULT_PALETTE: the 20 static colours, the first and last ten entries of
/// the 8bpp table.
inline constexpr std::array<u32, 20> kGdiStaticColors = [] {
    std::array<u32, 20> out{};
    for (u32 i = 0; i < 10; ++i) {
        out[i] = kGdiDefaultPalette8[i];
        out[10 + i] = kGdiDefaultPalette8[246 + i];
    }
    return out;
}();

}  // namespace vitreo::winapi::gdi
