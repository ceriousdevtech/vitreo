#pragma once
/// @file gdiplus_image.hpp
/// @brief GpImage / GpBitmap / GpMetafile, image attributes, cached bitmaps
/// and the pixel bridge the renderer draws through.
///
/// Contract: GdiPlusFlat.h / GdiPlusPixelFormats.h / GdiPlusImaging.h /
/// GdiPlusColorMatrix.h / GdiPlusImageAttributes.h, the Learn Bitmap, Image,
/// ImageAttributes and Graphics::DrawImage pages, and
/// tools/oracle_probe/gdiplus_image_probe.c (.expected = x64,
/// .x86.expected = x86) for every value Learn does not state. Notes and
/// measured deviations: .docs/contracts/gdiplus/image.md.

#include "winapi/graphics/gdiplus/gdiplus_core.hpp"
#include "winapi/graphics/gdiplus/gdiplus_codec_ref.hpp"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {
class GdiplusManager;
}

namespace vitreo::winapi::gdip {

struct GpGraphics;  // gdiplus_graphics.hpp

/// GDI+ PixelFormat values (GdiPlusPixelFormats.h).
enum PixelFormat : i32 {
    PixelFormatIndexed = 0x00010000,
    PixelFormatGDI = 0x00020000,
    PixelFormatAlpha = 0x00040000,
    PixelFormatPAlpha = 0x00080000,
    PixelFormatExtended = 0x00100000,
    PixelFormatCanonical = 0x00200000,
    PixelFormatUndefined = 0,
    PixelFormat1bppIndexed = 1 | (1 << 8) | PixelFormatIndexed | PixelFormatGDI,
    PixelFormat4bppIndexed = 2 | (4 << 8) | PixelFormatIndexed | PixelFormatGDI,
    PixelFormat8bppIndexed = 3 | (8 << 8) | PixelFormatIndexed | PixelFormatGDI,
    PixelFormat16bppGrayScale = 4 | (16 << 8) | PixelFormatExtended,
    PixelFormat16bppRGB555 = 5 | (16 << 8) | PixelFormatGDI,
    PixelFormat16bppRGB565 = 6 | (16 << 8) | PixelFormatGDI,
    PixelFormat16bppARGB1555 = 7 | (16 << 8) | PixelFormatAlpha | PixelFormatGDI,
    PixelFormat24bppRGB = 8 | (24 << 8) | PixelFormatGDI,
    PixelFormat32bppRGB = 9 | (32 << 8) | PixelFormatGDI,
    PixelFormat32bppARGB = 10 | (32 << 8) | PixelFormatAlpha | PixelFormatGDI | PixelFormatCanonical,
    PixelFormat32bppPARGB = 11 | (32 << 8) | PixelFormatAlpha | PixelFormatPAlpha | PixelFormatGDI,
    PixelFormat48bppRGB = 12 | (48 << 8) | PixelFormatExtended,
    PixelFormat64bppARGB = 13 | (64 << 8) | PixelFormatAlpha | PixelFormatCanonical | PixelFormatExtended,
    PixelFormat64bppPARGB = 14 | (64 << 8) | PixelFormatAlpha | PixelFormatPAlpha | PixelFormatExtended,
    PixelFormat32bppCMYK = 15 | (32 << 8),
};
[[nodiscard]] constexpr i32 pixel_format_bits(i32 format) { return (format >> 8) & 0xFF; }
/// The bits GDI+ checks: bits 24-31 of a PixelFormat are ignored (and kept
/// in what GdipGetImagePixelFormat reports), probe "fmtcheck".
[[nodiscard]] constexpr i32 pixel_format_canonical(i32 format) { return format & 0x00FFFFFF; }
/// One of the 15 SDK formats after masking bits 24-31.
[[nodiscard]] bool pixel_format_valid(i32 format);
/// 1/4/8bpp indexed.
[[nodiscard]] bool pixel_format_indexed(i32 format);
/// GDI+ stores but cannot convert or render these (16bppGrayScale and
/// 32bppCMYK: GetPixel/SetPixel InvalidParameter, a Graphics OutOfMemory,
/// only a same-format LockBits; probe "formats").
[[nodiscard]] bool pixel_format_opaque_storage(i32 format);
/// Bytes per row GDI+ allocates for @p width pixels (DWORD aligned).
[[nodiscard]] i32 pixel_format_stride(i32 width, i32 format);

/// GDI+ `ImageType`.
enum ImageType : i32 { ImageTypeUnknown = 0, ImageTypeBitmap = 1, ImageTypeMetafile = 2 };

/// ImageFlags (GdiPlusImaging.h).
enum ImageFlagsBits : u32 {
    ImageFlagsHasAlpha = 0x0002,
    ImageFlagsReadOnly = 0x00010000,
};

/// ImageLockMode (GdiPlusImaging.h).
enum ImageLockModeBits : u32 {
    ImageLockModeRead = 1,
    ImageLockModeWrite = 2,
    ImageLockModeUserInputBuf = 4,
};

/// An outstanding GdipBitmapLockBits. GDI+ keeps the lock's buffer and
/// rectangle itself; UnlockBits works from this record, not from the
/// BitmapData the caller passes back (probe "unlock_other").
struct BitmapLock {
    i32 x{0}, y{0}, width{0}, height{0};
    u32 mode{0};
    i32 format{0};           ///< as requested (bits 24-31 kept)
    GuestAddr scan0{0};      ///< what BitmapData.Scan0 received
    i32 stride{0};
    GuestAddr allocation{0}; ///< GDI+'s own buffer (0 for UserInputBuf / caller Scan0)
    bool user_buffer{false};
    /// The buffer holds the bitmap's own bits (same format): it is copied
    /// back on unlock whatever the mode, as GDI+'s direct pointer would.
    bool same_format{false};
};

/// A GDI+ image. Bitmaps hold pixels in their own PixelFormat, as GDI+
/// does, so GetPixel / LockBits round-trip exactly.
///
/// Extension rules for other areas (the codec area creates decoded bitmaps
/// and metafiles through this struct): set type/width/height/pixel_format/
/// dpi/flags/raw_format, fill bits+stride (top-down) and palette for a
/// bitmap; for a metafile set type = ImageTypeMetafile, bounds/bounds_unit
/// and physical_width/height, and install the metafile drawer
/// (set_metafile_drawer) so DrawImage can play it.
struct GpImage {
    ImageType type{ImageTypeBitmap};
    i32 width{0};
    i32 height{0};
    i32 pixel_format{PixelFormat32bppARGB};  ///< as created (bits 24-31 kept)
    float dpi_x{96.0f};
    float dpi_y{96.0f};
    u32 flags{0};                 ///< ImageFlags (GdipGetImageFlags)
    u8 raw_format[16]{};          ///< ImageFormat GUID (GdipGetImageRawFormat)
    /// Pixels, top-down, @p stride bytes per row, when owned host-side.
    std::vector<u8> bits;
    i32 stride{0};
    /// GdipCreateBitmapFromScan0 with a caller buffer, and
    /// GdipCreateBitmapFromGdiDib: the pixels ARE that guest memory (the
    /// caller keeps it alive); user_scan0 is the TOP row and stride may be
    /// negative (a bottom-up DIB, or a negative Scan0 stride).
    GuestAddr user_scan0{0};
    /// Palette: ARGB entries. Indexed formats always have one; any image
    /// can be given one by GdipSetImagePalette (probe "setpal32").
    std::vector<u32> palette;
    u32 palette_flags{0};
    i32 locked{0};                ///< LockBits outstanding (see lock)
    BitmapLock lock;
    /// GdipImageSetAbort: the guest GdiplusAbort* (0 = none).
    GuestAddr abort_object{0};
    /// Metafiles (type != Bitmap): GdipGetImageBounds' frame and unit, and
    /// GdipGetImageDimension's size, as the codec area decodes them.
    RectF bounds{};
    i32 bounds_unit{2};           ///< UnitPixel
    float physical_width{0};
    float physical_height{0};
    /// Codec area (gdiplus_codec.hpp): encoded source, frames, property
    /// items, save session, metafile records. Deep-copied with the image.
    CodecRef codec;
};

/// A fresh, zeroed host-owned bitmap of @p format with GDI+'s default
/// palette for an indexed format (probe "formats"). @p format must be valid.
[[nodiscard]] GpImage make_bitmap(i32 width, i32 height, i32 format);
/// GDI+'s default palette for @p format (empty for non-indexed) and its
/// PaletteFlags.
[[nodiscard]] std::vector<u32> default_palette(i32 format, u32* flags = nullptr);
/// The ImageFlags GDI+ reports for a bitmap of @p format: HasAlpha for an
/// alpha or indexed format (probe "formats": flags=2).
[[nodiscard]] u32 bitmap_flags_for(i32 format);

/// @p image's pixels in rectangle (x, y, w, h) as straight ARGB
/// (0xAARRGGBB), row by row. False for a metafile, a format GDI+ cannot
/// convert (16bppGrayScale, CMYK) or unreadable guest Scan0 memory.
[[nodiscard]] bool image_read_argb(Marshaler& m, const GpImage& image, i32 x, i32 y, i32 w, i32 h,
                                   std::vector<u32>& argb);
/// Store straight ARGB into rectangle (x, y, w, h) of @p image in its own
/// format (indexed formats map to the nearest palette entry).
[[nodiscard]] bool image_write_argb(Marshaler& m, GpImage& image, i32 x, i32 y, i32 w, i32 h,
                                    const std::vector<u32>& argb);

/// A 32bpp premultiplied B,G,R,A copy of an image's pixels, the format the
/// renderer draws in (CoreGraphics kCGImageAlphaPremultipliedFirst |
/// kCGBitmapByteOrder32Little).
struct RenderPixels {
    std::vector<u8> bgra;
    i32 width{0};
    i32 height{0};
};
/// Convert @p image's pixels to RenderPixels. False for an image with no
/// pixel access (a metafile, or a format the converter refuses).
[[nodiscard]] bool image_to_render(Marshaler& m, const GpImage& image, RenderPixels& out);
/// Store @p px back into @p image in its own format (quantising as GDI+
/// does for formats narrower than 32bpp ARGB).
[[nodiscard]] bool render_to_image(Marshaler& m, GpImage& image, const RenderPixels& px);
/// Straight ARGB -> premultiplied BGRA (GDI+ rounding: (c*a + 127) / 255).
void argb_to_render(const u32* argb, usize count, u8* bgra);

// ---- Image attributes ------------------------------------------------------

/// ColorAdjustType (GdiPlusColorMatrix.h): Default .. Text are categories;
/// Count and Any are rejected.
enum ColorAdjustType : i32 {
    ColorAdjustTypeDefault = 0,
    ColorAdjustTypeBitmap = 1,
    ColorAdjustTypeBrush = 2,
    ColorAdjustTypePen = 3,
    ColorAdjustTypeText = 4,
    ColorAdjustTypeCount = 5,
};

/// ColorMatrix: 5x5 REAL, row-vector convention (out_j = sum_i in_i * m[i][j]).
struct ColorMatrix5 {
    float m[5][5]{};
};

/// One category's colour adjustments (GdipSetImageAttributes*).
struct ColorAdjust {
    /// Any GdipSetImageAttributes* call on this category makes it used in
    /// place of Default (even NoOp(FALSE) or ToIdentity); Reset clears it
    /// (probe "noop_off", "reset_bitmap").
    bool set{false};
    bool noop{false};
    bool matrix_enabled{false};
    ColorMatrix5 matrix;
    ColorMatrix5 gray_matrix;
    i32 matrix_flags{0};          ///< ColorMatrixFlags: 0 default, 1 SkipGrays, 2 AltGray
    bool threshold_enabled{false};
    float threshold{0};
    bool gamma_enabled{false};
    float gamma{1};
    bool key_enabled{false};
    u32 key_low{0}, key_high{0};
    bool channel_enabled{false};
    i32 channel{0};               ///< ColorChannelFlagsC/M/Y/K
    bool profile_enabled{false};
    std::vector<u8> profile;      ///< CMYK output profile (ICC bytes), empty = generic CMYK
    bool remap_enabled{false};
    std::vector<std::pair<u32, u32>> remap;  ///< (old, new) ARGB
};

struct GpImageAttributes {
    std::array<ColorAdjust, 5> adjust{};
    /// GdipSetImageAttributesWrapMode: what lies outside the image when a
    /// source rectangle extends past it. A fresh object clamps to
    /// transparent, as drawing without attributes does (probe "wrap_none").
    i32 wrap_mode{4};             ///< WrapModeClamp
    u32 wrap_color{0};
    bool wrap_clamp{false};
    bool cached_background{false};
};

/// The adjustments drawing an object of @p type uses: its own category when
/// set, else Default (probe "default_inv_bitmap_swap", "brush_only").
[[nodiscard]] const ColorAdjust& effective_adjust(const GpImageAttributes& ia, i32 type);
/// Apply @p adjust to straight ARGB pixels in place, in GDI+'s order:
/// remap, colour key, colour matrix (gray handling), gamma, threshold,
/// output channel (probe "remap_key808080", "thr0.4_gamma0.5", ...).
void apply_color_adjust(const ColorAdjust& adjust, u32* argb, usize count);

// ---- Cached bitmaps --------------------------------------------------------

struct GpCachedBitmap {
    RenderPixels pixels;  ///< premultiplied BGRA snapshot of the bitmap
};

// ---- Hooks other areas install ----------------------------------------------

/// Draw a non-bitmap image (metafile) so that its source rectangle
/// @p src (in the image's own units as GdipGetImageBounds reports them)
/// lands on the world-space parallelogram @p dst (top-left, top-right,
/// bottom-left). Installed by the codec area.
using MetafileDrawer = Status (*)(GdiplusManager& mgr, Marshaler& m, GpGraphics& g, GpImage& image,
                                  const PointF dst[3], const RectF& src,
                                  const GpImageAttributes* attributes);
void set_metafile_drawer(MetafileDrawer drawer);
/// Apply a CGpEffect (handle @p effect, the codec area's object) to straight
/// ARGB pixels of a @p width x @p height region in place.
using EffectApplier = Status (*)(GdiplusManager& mgr, u64 effect, std::vector<u32>& argb, i32 width,
                                 i32 height);
void set_effect_applier(EffectApplier applier);
/// GdipFindFirstImageItem / GdipFindNextImageItem for a decoded image:
/// fill the guest ImageItemData at @p item. Installed by the codec area; a
/// plain bitmap has no items (InvalidParameter, probe "finditem").
using ImageItemFinder = Status (*)(GdiplusManager& mgr, Marshaler& m, GpImage& image, bool first,
                                   GuestAddr item);
void set_image_item_finder(ImageItemFinder finder);

// ---- The area's translation units ----------------------------------------------

void register_image_attributes(GdiplusManager& mgr, HandlerMap& map);  ///< gdiplus_image_attributes.cpp
void register_image_draw(GdiplusManager& mgr, HandlerMap& map);        ///< gdiplus_image_draw.cpp

} // namespace vitreo::winapi::gdip
