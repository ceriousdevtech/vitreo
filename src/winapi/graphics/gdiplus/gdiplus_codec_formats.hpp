#pragma once
/// @file gdiplus_codec_formats.hpp
/// @brief The codec area's file-format layer: decoders and encoders for the
/// GDI+ built-in codecs, independent of guest memory.
///
/// Decoded pixel formats, flags, property-item sets and their order follow
/// the winoracle probe (gdiplus_codec_probe.c); the file layouts follow the
/// format specifications (BMP: Learn BITMAPINFOHEADER/BITMAPV5HEADER; PNG:
/// W3C PNG; GIF89a; TIFF 6.0; EXIF 2.3; JPEG ITU T.81 / JFIF).

#include "winapi/graphics/gdiplus/gdiplus_codec.hpp"

namespace vitreo::winapi::gdip::fmt {

/// The codec whose signature @p bytes carry (the decoder list's patterns).
[[nodiscard]] std::optional<SourceFormat> sniff(std::span<const u8> bytes);

/// A whole decoded image: frame @p index of a source, plus the frame layout.
struct Decoded {
    DecodedFrame frame;
    Guid dimension{kFrameDimensionPage};
    u32 frame_count{1};
};

/// Decode frame @p index of @p src (already sniffed as @p format). Status
/// is what GdipLoadImageFrom* reports for unreadable data.
[[nodiscard]] Status decode(SourceFormat format, std::span<const u8> src, u32 index, Decoded& out);

/// The embedded EXIF thumbnail of a JPEG (property 0x501B), decoded.
[[nodiscard]] bool decode_thumbnail(std::span<const u8> jpeg, DecodedFrame& out);

// ---- Encoding ------------------------------------------------------------------

/// One frame handed to an encoder: non-premultiplied ARGB plus the source
/// format/palette (encoders choose their file layout from it).
struct EncodeFrame {
    i32 width{0};
    i32 height{0};
    i32 source_format{PixelFormat32bppARGB};
    std::vector<u32> argb;
    std::vector<u32> palette;
    std::vector<u8> raw;          ///< 64bppARGB source rows (BMP keeps them verbatim)
    float dpi_x{96.0f};
    float dpi_y{96.0f};
    std::vector<Property> properties;
};

struct EncodeOptions {
    i32 jpeg_quality{-1};         ///< -1: the encoder's default
    i32 tiff_compression{-1};     ///< EncoderValueCompression*, -1 default
    i32 color_depth{0};           ///< 0: from the source
    bool save_as_cmyk{false};
};

[[nodiscard]] Status encode_bmp(const EncodeFrame& f, std::vector<u8>& out);
[[nodiscard]] Status encode_png(const EncodeFrame& f, std::vector<u8>& out);
[[nodiscard]] Status encode_gif(const std::vector<EncodeFrame>& frames, std::vector<u8>& out);
[[nodiscard]] Status encode_tiff(const std::vector<EncodeFrame>& frames, const EncodeOptions& o, std::vector<u8>& out);
[[nodiscard]] Status encode_jpeg(const EncodeFrame& f, const EncodeOptions& o, std::vector<u8>& out);

// ---- ImageIO (gdiplus_codec_imageio.mm) ------------------------------------------

/// Frame @p index of an ImageIO-readable image as non-premultiplied ARGB.
[[nodiscard]] bool imageio_decode(std::span<const u8> src, u32 index, i32& width, i32& height,
                                  std::vector<u32>& argb);
[[nodiscard]] u32 imageio_count(std::span<const u8> src);
/// Baseline JPEG of @p argb at @p quality (0..100), JFIF density @p dpi.
[[nodiscard]] bool imageio_encode_jpeg(const std::vector<u32>& argb, i32 width, i32 height, i32 quality,
                                       float dpi_x, float dpi_y, std::vector<u8>& out);
/// Scale @p argb to @p w x @p h with high-quality interpolation (thumbnails).
[[nodiscard]] bool imageio_scale(const std::vector<u32>& argb, i32 width, i32 height, i32 w, i32 h,
                                 std::vector<u32>& out);

/// The GIF encoder's 256-entry palette for non-indexed images: a 6x7x6
/// R,G,B cube (blue fastest), then index 252 transparent and 253-255 black
/// (the layout the probe's r2.gifpalette line shows).
[[nodiscard]] const std::vector<u32>& gif_halftone_palette();

}  // namespace vitreo::winapi::gdip::fmt
