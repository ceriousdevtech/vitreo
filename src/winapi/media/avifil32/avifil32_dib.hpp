#pragma once

/// @file avifil32_dib.hpp
/// @brief DIB helpers for AVIStreamGetFrame: strides, colour tables,
/// AVIPALCHANGE application and the BI_RLE8 decoder.
///
/// BI_RLE8 is the documented "Bitmap Compression" encoding (Learn, wingdi
/// BITMAPINFOHEADER / "Bitmap Compression"): encoded runs, absolute runs
/// padded to a WORD, and the escapes 0 = end of line, 1 = end of bitmap,
/// 2 = delta (dx, dy). Windows decodes it through msrle32; Vitreo has no
/// guest copy of that in-box codec, so the decoding is done here.

#include <vitreo/common.hpp>

#include <vector>

namespace vitreo::winapi::avifil32 {

struct BitmapInfoHeader {
    u32 size{0};
    i32 width{0};
    i32 height{0};
    u16 planes{0};
    u16 bit_count{0};
    u32 compression{0};
    u32 size_image{0};
    i32 x_ppm{0};
    i32 y_ppm{0};
    u32 clr_used{0};
    u32 clr_important{0};
};

/// Parse the leading BITMAPINFOHEADER of @p bytes (false when shorter).
[[nodiscard]] bool read_bih(const std::vector<u8>& bytes, BitmapInfoHeader& out);
[[nodiscard]] bool read_bih(const u8* bytes, usize n, BitmapInfoHeader& out);
void write_bih(u8* dst, const BitmapInfoHeader& bih);

/// Bytes per DIB row (DWORD aligned).
[[nodiscard]] u32 dib_stride(i32 width, u16 bit_count);

/// Colour-table entries a packed DIB with this header carries.
[[nodiscard]] u32 dib_color_entries(const BitmapInfoHeader& bih);

/// Apply an AVIPALCHANGE to the colour table that follows the
/// BITMAPINFOHEADER in @p format (bNumEntries 0 = 256).
void apply_palette_change(std::vector<u8>& format, const std::vector<u8>& palchange);

/// Decode one BI_RLE8 frame onto @p dst (bottom-up rows of @p stride bytes).
/// Pixels the frame does not touch keep their value, which is what a delta
/// frame relies on.
void decode_rle8(const u8* src, usize n, u8* dst, u32 stride, u32 width, u32 height);

} // namespace vitreo::winapi::avifil32
