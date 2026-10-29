#pragma once
/// @file gdiplus_image_internal.hpp
/// @brief Shared helpers of the GDI+ image area's translation units
/// (gdiplus_image*.cpp). Not for other areas: they use gdiplus_image.hpp.

#include "winapi/graphics/gdiplus/gdiplus_image.hpp"

#include <array>
#include <vector>

namespace vitreo::winapi {
class GdiplusManager;
class Marshaler;
}

namespace vitreo::winapi::gdip {

// ---- Rows --------------------------------------------------------------------

/// Row @p y of @p image: a pointer into the host bits, or @p scratch filled
/// from the guest Scan0 memory. Null when the memory cannot be read.
[[nodiscard]] const u8* image_row(Marshaler& m, const GpImage& image, i32 y, std::vector<u8>& scratch);
/// Writable row @p y: a pointer into the host bits, or @p scratch filled
/// from guest memory (commit with image_row_commit).
[[nodiscard]] u8* image_row_mut(Marshaler& m, GpImage& image, i32 y, std::vector<u8>& scratch);
/// Store @p scratch back to a guest-backed row (no-op for host bits).
[[nodiscard]] bool image_row_commit(Marshaler& m, GpImage& image, i32 y, const std::vector<u8>& scratch);
/// Bytes of one row of @p image's pixels (|stride|).
[[nodiscard]] i32 image_row_bytes(const GpImage& image);

// ---- Palettes and quantisation ---------------------------------------------------

/// Nearest-palette mapping as GDI+ does it (probe "setget" / "lockconvert"
/// / "convert"): each channel is reduced to 4 bits (v >> 4, rebuilt as
/// (v >> 4) * 17), then the palette entry nearest in RGB (squared
/// distance, alpha ignored, ties to the lowest index) is chosen.
class Quantizer {
public:
    explicit Quantizer(const std::vector<u32>& palette);
    [[nodiscard]] u8 index(u32 argb);
    /// Nearest entry to exact RGB (no 4-bit cell): error diffusion.
    [[nodiscard]] u8 nearest_exact(i32 r, i32 g, i32 b) const;
    [[nodiscard]] const std::vector<u32>& palette() const { return m_palette; }

private:
    std::vector<u32> m_palette;
    std::array<i16, 4096> m_cache{};
};

/// Convert @p count pixels of one row from (@p sfmt, @p spal) starting at
/// pixel @p sx of @p src into (@p dfmt, @p dpal) at pixel @p dx of @p dst.
/// @p q quantises into @p dpal when @p dfmt is indexed (may be null
/// otherwise). False when GDI+ refuses the conversion (16bppGrayScale or
/// CMYK to or from another format).
[[nodiscard]] bool convert_row(i32 sfmt, const std::vector<u32>& spal, const u8* src, i32 sx, i32 dfmt,
                               const std::vector<u32>& dpal, u8* dst, i32 dx, i32 count, Quantizer* q);
/// True when convert_row can convert @p sfmt to @p dfmt.
[[nodiscard]] bool conversion_supported(i32 sfmt, i32 dfmt);

/// Straight ARGB of one pixel / store one pixel (bpp >= 8 or indexed).
[[nodiscard]] u32 decode_pixel(i32 format, const std::vector<u32>& palette, const u8* row, i32 x);
void encode_pixel(i32 format, u8* row, i32 x, u32 argb, Quantizer* q);

/// Index of pixel @p x of an indexed row (1/4/8 bpp) / store one.
[[nodiscard]] u32 get_index(const u8* row, i32 x, i32 bpp);
void set_index(u8* row, i32 x, i32 bpp, u32 index);

/// GdipInitializePalette's fixed tables (PaletteType 2..10, probe
/// "initpal"); false for a type without a fixed table.
[[nodiscard]] bool fixed_palette(i32 type, std::vector<u32>& out);
/// GdipCreateHalftonePalette's 256 entries (built from the halftone
/// palette's definition): 0x00RRGGBB, PALETTEENTRY flags in bits 24-31.
[[nodiscard]] const std::array<u32, 256>& halftone_palette_entries();

/// Median-cut palette of @p argb with at most @p colors entries (alpha
/// ignored). Exact colours when the image has no more than @p colors.
[[nodiscard]] std::vector<u32> optimal_palette(const std::vector<u32>& argb, i32 colors);

/// GdipBitmapConvertFormat's pixel work: @p argb (width x height) into the
/// bits of an indexed or 16bpp target with @p dither (DitherType),
/// @p palette for indexed targets and @p alpha_threshold (0..255: an alpha
/// below it maps to the palette's transparent entry). Rows are
/// pixel_format_stride bytes.
/// @p palette_type (PaletteType) gives an ordered dither its level spacing.
void dither_into(const std::vector<u32>& argb, i32 width, i32 height, i32 format, i32 dither,
                 i32 palette_type, const std::vector<u32>& palette, i32 alpha_threshold,
                 std::vector<u8>& bits);

/// Rotate/flip @p image's pixels (RotateFlipType 0..7) into host bits.
[[nodiscard]] bool rotate_flip_image(Marshaler& m, GpImage& image, i32 type);

// ---- 64bpp (linear, 0..8192) ---------------------------------------------------------

/// sRGB 8-bit -> 13-bit linear (probe "to64": round(8192 * sRGB^-1(v/255))).
[[nodiscard]] u16 srgb_to_linear13(u8 v);
/// 13-bit linear -> sRGB 8-bit (probe "from64"), saturating above 8192.
[[nodiscard]] u8 linear13_to_srgb(u32 v);

// ---- Guest memory ------------------------------------------------------------------

/// Process-heap guest memory owned until destruction (or release()).
class GuestScratch {
public:
    GuestScratch(GdiplusManager& mgr, u64 size, bool zero = true);
    ~GuestScratch();
    GuestScratch(const GuestScratch&) = delete;
    GuestScratch& operator=(const GuestScratch&) = delete;
    [[nodiscard]] GuestAddr addr() const { return m_addr; }
    GuestAddr release() { const GuestAddr a = m_addr; m_addr = 0; return a; }

private:
    GdiplusManager& m_mgr;
    GuestAddr m_addr{0};
};
/// Free process-heap memory a GuestScratch released.
void guest_free(GdiplusManager& mgr, GuestAddr addr);

/// Drop an outstanding lock without writing it back (RotateFlip and
/// ConvertFormat replace the pixels under it; probe "locked:").
void discard_lock(GdiplusManager& mgr, GpImage& image);

/// The status for a handle that did not resolve to the expected class: a
/// live object of another class is ObjectBusy (probe "wrongclass"), NULL
/// or an unknown value InvalidParameter.
[[nodiscard]] Status missing_object(GdiplusManager& mgr, u64 h);

/// GDI+ rounding for REAL -> INT parameters: floor(v + 0.5).
[[nodiscard]] i32 gdip_round(float v);

}  // namespace vitreo::winapi::gdip
