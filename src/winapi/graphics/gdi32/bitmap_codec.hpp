#pragma once
/// @file bitmap_codec.hpp
/// @brief Scan-line formats shared by every GDI pixel-transfer export.
///
/// DIBs (CreateDIBSection, GetDIBits, SetDIBits, SetDIBitsToDevice,
/// StretchDIBits) and device-dependent bitmaps (CreateBitmap, GetBitmapBits,
/// SetBitmapBits) all describe pixels as packed scan lines of 1, 4, 8, 16, 24
/// or 32 bits. GDI keeps every surface as RGBA8 internally; this is the one
/// place that converts between the two, so each export gets the same rules:
///
///  - 32bpp BI_RGB carries its fourth byte through verbatim; every other
///    format decodes with 0 there (tools/oracle_probe/s3gdi_blit_probe).
///  - Narrow BI_BITFIELDS channels widen by bit replication, 555/565 included
///    (s3gdi_blit_probe "SDIB 16bpp", "SDIB 32bpp narrow bitfields").
///  - Encoding truncates wide channels and picks the nearest palette entry
///    for indexed formats (s3gdi_bitmap_probe "gdib16", "gdib4", "gdib1").

#include <vitreo/common.hpp>

#include <array>
#include <span>
#include <utility>
#include <vector>

#include "winapi/graphics/gdi32/default_palettes.hpp"

namespace vitreo { class GuestMemory; }

namespace vitreo::winapi::gdi {

#include "winapi/graphics/gdi32/gdi_oracle_tables.inc"

inline constexpr u32 kBiRgb = 0;
inline constexpr u32 kBiRle8 = 1;
inline constexpr u32 kBiRle4 = 2;
inline constexpr u32 kBiBitfields = 3;
inline constexpr u32 kBiJpeg = 4;
inline constexpr u32 kBiPng = 5;

inline constexpr u32 kDibRgbColors = 0;
inline constexpr u32 kDibPalColors = 1;

/// CreateBitmap depth: nPlanes * nBitCount rounded up to a surface format.
/// 0 when the product exceeds 32, which CreateBitmap rejects with
/// ERROR_INVALID_PARAMETER (s3gdi_bitmap_probe "CreateBitmap formats").
[[nodiscard]] constexpr u16 round_ddb_bpp(u32 bits) noexcept {
    if (bits <= 1) return 1;
    if (bits <= 4) return 4;
    if (bits <= 8) return 8;
    if (bits <= 16) return 16;
    if (bits <= 24) return 24;
    if (bits <= 32) return 32;
    return 0;
}

/// Device-dependent bitmap rows are WORD aligned (BITMAP::bmWidthBytes).
[[nodiscard]] constexpr u32 ddb_stride(i32 width, u32 bpp) noexcept {
    return ((static_cast<u32>(width) * bpp + 15u) / 16u) * 2u;
}

/// DIB rows are DWORD aligned.
[[nodiscard]] constexpr u32 dib_stride(i32 width, u32 bpp) noexcept {
    return ((static_cast<u32>(width) * bpp + 31u) / 32u) * 4u;
}

/// How one scan line's bytes map to RGBA.
struct PixelFormat {
    u16 bpp = 32;
    /// BI_BITFIELDS masks in use (16/32bpp). BI_RGB 16bpp is 555 and BI_RGB
    /// 32bpp is BGRX; neither sets this.
    bool bitfields = false;
    std::array<u32, 3> masks{};  ///< red, green, blue
    std::vector<u32> palette;    ///< 0x00RRGGBB, for bpp <= 8
};

/// The format of a device-dependent bitmap of @p bpp: 555 for 16bpp and the
/// display's default colour tables for 1/4/8bpp (s3gdi_info_probe).
[[nodiscard]] PixelFormat ddb_pixel_format(u16 bpp);

/// Decode @p count pixels starting at pixel @p first of a scan line.
void decode_row(const PixelFormat& format, const u8* row, u8* rgba, i32 count,
                i32 first = 0);

/// Encode @p count RGBA pixels into a scan line starting at pixel @p first.
/// Bits of partially covered bytes outside the run are preserved.
void encode_row(const PixelFormat& format, const u8* rgba, u8* row, i32 count,
                i32 first = 0);

/// Nearest palette entry to an RGB triple by squared distance, first wins on
/// a tie. Matches Windows exactly for 1bpp and 4bpp; 8bpp GetDIBits uses an
/// inverse colour table this does not reproduce for every colour.
[[nodiscard]] u32 nearest_palette_index(std::span<const u32> palette, u8 r, u8 g,
                                        u8 b) noexcept;

/// A BITMAPINFO read from guest memory.
struct DibHeader {
    GuestAddr address = 0;
    u32 header_size = 0;  ///< biSize; 12 is a BITMAPCOREHEADER
    i32 width = 0;
    i32 height = 0;       ///< signed: negative is top-down
    u16 planes = 1;
    u16 bpp = 0;
    u32 compression = 0;
    u32 size_image = 0;
    i32 x_pels_per_meter = 0;
    i32 y_pels_per_meter = 0;
    u32 clr_used = 0;
    u32 clr_important = 0;
    std::array<u32, 3> masks{};  ///< as read, when BI_BITFIELDS

    [[nodiscard]] bool core() const noexcept { return header_size == 12; }
    [[nodiscard]] bool top_down() const noexcept { return height < 0; }
    [[nodiscard]] i32 abs_height() const noexcept { return height < 0 ? -height : height; }
    [[nodiscard]] u32 stride() const noexcept { return dib_stride(width, bpp); }
    /// Entries in the colour table that follows the header (and the masks).
    [[nodiscard]] u32 color_count() const noexcept;
    /// Guest address of the colour table.
    [[nodiscard]] GuestAddr color_table() const noexcept;
    /// Guest address of the three BI_BITFIELDS DWORDs (inside a V4/V5 header,
    /// after a BITMAPINFOHEADER).
    [[nodiscard]] GuestAddr mask_address() const noexcept;
};

/// Read a BITMAPINFOHEADER, V4/V5 header or BITMAPCOREHEADER, and the
/// BI_BITFIELDS masks. False when unreadable or biSize is not a header size.
[[nodiscard]] bool read_dib_header(GuestMemory& memory, GuestAddr bmi, DibHeader& out);

/// True when @p h describes an uncompressed (BI_RGB/BI_BITFIELDS) DIB this
/// codec can decode: bpp 1/4/8/16/24/32, bitfields only at 16/32bpp with a
/// non-zero mask set.
[[nodiscard]] bool dib_is_decodable(const DibHeader& h) noexcept;

/// The pixel format a DIB header describes. For DIB_RGB_COLORS the palette is
/// read from the colour table; for DIB_PAL_COLORS each WORD indexes
/// @p logical_palette (0x00RRGGBB). False when the table is unreadable or an
/// index is out of range.
[[nodiscard]] bool dib_pixel_format(GuestMemory& memory, const DibHeader& h, u32 usage,
                                    std::span<const u32> logical_palette,
                                    PixelFormat& out);

}  // namespace vitreo::winapi::gdi
