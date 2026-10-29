// SPDX-License-Identifier: LGPL-2.1-only
/// @file bmp_decode.cpp
/// @brief BMP file to RGBA decoding shared by GDI, LoadImage and WIC.
///
/// Layout reference: BITMAPFILEHEADER / BITMAPINFOHEADER / BITMAPCOREHEADER
/// https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader

#include "bmp_decode.hpp"

namespace vitreo::winapi {

std::optional<DecodedBmp> decode_bmp_file(std::span<const u8> d) {
    if (d.size() < 14 + 12) return std::nullopt;
    if (d[0] != 'B' || d[1] != 'M') return std::nullopt;

    const auto r16 = [&](size_t off) -> u32 {
        return static_cast<u32>(d[off]) | (static_cast<u32>(d[off + 1]) << 8);
    };
    const auto r32 = [&](size_t off) -> u32 {
        return static_cast<u32>(d[off]) | (static_cast<u32>(d[off + 1]) << 8) |
               (static_cast<u32>(d[off + 2]) << 16) |
               (static_cast<u32>(d[off + 3]) << 24);
    };

    // Pixel data offset from BITMAPFILEHEADER, DIB header size after it.
    const u32 px_offset = r32(10);
    const u32 hdr_size = r32(14);

    i32 w = 0;
    i32 h = 0;
    u16 bpp = 0;
    const u32 palette_offset = 14 + hdr_size;
    u32 palette_entry_size = 4; // RGBQUAD for BITMAPINFOHEADER and later

    if (hdr_size == 12) { // BITMAPCOREHEADER
        w = static_cast<i32>(r16(18));
        h = static_cast<i32>(r16(20));
        bpp = static_cast<u16>(r16(24));
        palette_entry_size = 3; // RGBTRIPLE
    } else if (hdr_size >= 40 && d.size() >= 14 + 40) { // BITMAPINFOHEADER+
        w = static_cast<i32>(r32(14 + 4));
        h = static_cast<i32>(r32(14 + 8));
        bpp = static_cast<u16>(r16(14 + 14));
    } else {
        return std::nullopt;
    }

    const bool bottom_up = h > 0;
    const i32 abs_h = bottom_up ? h : -h;
    if (w <= 0 || abs_h <= 0 || w > 16384 || abs_h > 16384) return std::nullopt;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) {
        return std::nullopt;
    }

    DecodedBmp out;
    out.width = w;
    out.height = abs_h;
    out.bits_per_pixel = bpp;
    out.rgba.resize(static_cast<size_t>(w) * abs_h * 4);

    // Rows are padded to a 4-byte boundary.
    const u32 bits_per_row = static_cast<u32>(w) * bpp;
    const u32 row_bytes = (bits_per_row + 31) / 32 * 4;

    // Palette for indexed depths, stored as R,G,B triples.
    std::vector<u8> palette;
    if (bpp <= 8) {
        const u32 num_colors = 1u << bpp;
        palette.assign(num_colors * 3, 0);
        for (u32 c = 0; c < num_colors; ++c) {
            const size_t off = palette_offset + c * palette_entry_size;
            if (off + palette_entry_size > d.size()) break;
            palette[c * 3 + 2] = d[off + 0]; // blue
            palette[c * 3 + 1] = d[off + 1]; // green
            palette[c * 3 + 0] = d[off + 2]; // red
        }
        if (bpp == 1) {
            const auto is = [&](u32 c, u8 v) {
                return palette[c * 3] == v && palette[c * 3 + 1] == v &&
                       palette[c * 3 + 2] == v;
            };
            out.black_white_table = (is(0, 0x00) && is(1, 0xFF)) ||
                                    (is(0, 0xFF) && is(1, 0x00));
        }
    }

    for (i32 row = 0; row < abs_h; ++row) {
        const i32 src_row = bottom_up ? (abs_h - 1 - row) : row;
        const size_t src_off =
            px_offset + static_cast<size_t>(src_row) * row_bytes;
        u8* dst = &out.rgba[static_cast<size_t>(row) * w * 4];

        for (i32 col = 0; col < w; ++col) {
            u8 r = 0, g = 0, b = 0;
            switch (bpp) {
            case 1: {
                const size_t byte_off = src_off + col / 8;
                if (byte_off < d.size()) {
                    const u8 bit = (d[byte_off] >> (7 - (col & 7))) & 1;
                    r = palette[bit * 3 + 0];
                    g = palette[bit * 3 + 1];
                    b = palette[bit * 3 + 2];
                }
                break;
            }
            case 4: {
                const size_t byte_off = src_off + col / 2;
                if (byte_off < d.size()) {
                    const u8 nibble =
                        (col & 1) ? (d[byte_off] & 0x0F) : (d[byte_off] >> 4);
                    r = palette[nibble * 3 + 0];
                    g = palette[nibble * 3 + 1];
                    b = palette[nibble * 3 + 2];
                }
                break;
            }
            case 8: {
                const size_t byte_off = src_off + col;
                if (byte_off < d.size()) {
                    const u8 idx = d[byte_off];
                    r = palette[idx * 3 + 0];
                    g = palette[idx * 3 + 1];
                    b = palette[idx * 3 + 2];
                }
                break;
            }
            case 24: {
                const size_t byte_off = src_off + static_cast<size_t>(col) * 3;
                if (byte_off + 2 < d.size()) {
                    b = d[byte_off + 0];
                    g = d[byte_off + 1];
                    r = d[byte_off + 2];
                }
                break;
            }
            case 32: {
                const size_t byte_off = src_off + static_cast<size_t>(col) * 4;
                if (byte_off + 3 < d.size()) {
                    b = d[byte_off + 0];
                    g = d[byte_off + 1];
                    r = d[byte_off + 2];
                }
                break;
            }
            default:
                break;
            }
            dst[col * 4 + 0] = r;
            dst[col * 4 + 1] = g;
            dst[col * 4 + 2] = b;
            dst[col * 4 + 3] = 0xFF;
        }
    }
    return out;
}

u16 compatible_bitmap_bpp(const DecodedBmp& bmp, u16 display_bpp) noexcept {
    if (bmp.bits_per_pixel == 1 && !bmp.black_white_table) return display_bpp;
    return bmp.bits_per_pixel;
}

} // namespace vitreo::winapi
