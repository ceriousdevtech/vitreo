// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo -- avifil32 DIB helpers and the BI_RLE8 decoder.
//
// Bitmap Compression (BI_RLE8):
// https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-compression

#include "winapi/media/avifil32/avifil32_dib.hpp"
#include "winapi/media/avifil32/avifil32_contract.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::avifil32 {

namespace {
u32 le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}
u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
void put32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}
void put16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
} // namespace

bool read_bih(const u8* b, usize n, BitmapInfoHeader& out) {
    if (n < kBitmapInfoHeaderSize) return false;
    out.size = le32(b);
    out.width = static_cast<i32>(le32(b + 4));
    out.height = static_cast<i32>(le32(b + 8));
    out.planes = le16(b + 12);
    out.bit_count = le16(b + 14);
    out.compression = le32(b + 16);
    out.size_image = le32(b + 20);
    out.x_ppm = static_cast<i32>(le32(b + 24));
    out.y_ppm = static_cast<i32>(le32(b + 28));
    out.clr_used = le32(b + 32);
    out.clr_important = le32(b + 36);
    return true;
}

bool read_bih(const std::vector<u8>& bytes, BitmapInfoHeader& out) {
    return read_bih(bytes.data(), bytes.size(), out);
}

void write_bih(u8* d, const BitmapInfoHeader& h) {
    put32(d, h.size);
    put32(d + 4, static_cast<u32>(h.width));
    put32(d + 8, static_cast<u32>(h.height));
    put16(d + 12, h.planes);
    put16(d + 14, h.bit_count);
    put32(d + 16, h.compression);
    put32(d + 20, h.size_image);
    put32(d + 24, static_cast<u32>(h.x_ppm));
    put32(d + 28, static_cast<u32>(h.y_ppm));
    put32(d + 32, h.clr_used);
    put32(d + 36, h.clr_important);
}

u32 dib_stride(i32 width, u16 bit_count) {
    const u64 w = static_cast<u64>(width < 0 ? -static_cast<i64>(width) : width);
    return static_cast<u32>(((w * bit_count + 31) / 32) * 4);
}

u32 dib_color_entries(const BitmapInfoHeader& bih) {
    if (bih.compression == BI_BITFIELDS) return 3;
    if (bih.clr_used != 0) return bih.clr_used;
    return bih.bit_count <= 8 ? (1u << bih.bit_count) : 0;
}

void apply_palette_change(std::vector<u8>& format, const std::vector<u8>& pc) {
    if (pc.size() < 4) return;
    const u32 first = pc[0];
    u32 count = pc[1];
    if (count == 0) count = 256;
    const usize table = kBitmapInfoHeaderSize;
    for (u32 i = 0; i < count; ++i) {
        const usize src = 4 + static_cast<usize>(i) * 4;
        const usize dst = table + static_cast<usize>(first + i) * 4;
        if (src + 4 > pc.size() || dst + 4 > format.size()) break;
        // PALETTEENTRY {peRed, peGreen, peBlue, peFlags} -> RGBQUAD {B, G, R, 0}
        format[dst + 0] = pc[src + 2];
        format[dst + 1] = pc[src + 1];
        format[dst + 2] = pc[src + 0];
        format[dst + 3] = 0;
    }
}

void decode_rle8(const u8* src, usize n, u8* dst, u32 stride, u32 width, u32 height) {
    u32 x = 0, y = 0;
    usize i = 0;
    auto put = [&](u8 v) {
        if (x < width && y < height) dst[static_cast<usize>(y) * stride + x] = v;
        ++x;
    };
    while (i + 2 <= n) {
        const u8 a = src[i++];
        const u8 b = src[i++];
        if (a != 0) {
            for (u32 k = 0; k < a; ++k) put(b);
            continue;
        }
        if (b == 0) { // end of line
            x = 0;
            ++y;
        } else if (b == 1) { // end of bitmap
            return;
        } else if (b == 2) { // delta
            if (i + 2 > n) return;
            x += src[i++];
            y += src[i++];
        } else { // absolute run of b pixels, padded to a WORD
            for (u32 k = 0; k < b && i < n; ++k) put(src[i++]);
            if (b & 1) ++i;
        }
        if (y >= height) return;
    }
}

} // namespace vitreo::winapi::avifil32
