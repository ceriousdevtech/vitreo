// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_image_pixels.cpp
/// @brief GDI+ pixel formats: storage, conversion, quantisation, palettes,
/// dithering, rotation, and the renderer bridge (image_to_render /
/// render_to_image).
///
/// Every conversion rule below is measured (tools/oracle_probe/
/// gdiplus_image_probe.c, sections "formats", "lockconvert", "convert",
/// "initpal"):
///  * 16bpp: 5/6-bit truncation, expanded as (v << 3) | (v >> 2) (555) and
///    (g << 2) | (g >> 4) (565 green); ARGB1555 alpha bit = alpha >= 128.
///  * 32bppPARGB: premultiply (c*a + 127) / 255, unpremultiply c*255/a
///    truncated (SetPixel 80ff4020 reads back 80ff3f1f).
///  * 32bppRGB keeps the source DWORD (its alpha byte is not cleared) and
///    reads as opaque.
///  * 48/64bpp: B,G,R(,A) WORDs, colour LINEAR on 0..8192 through the exact
///    sRGB curve (rounded both ways); alpha a*8192/255 and back a*255/8192,
///    both truncated; PARGB64 premultiplies (c*a) >> 13.
///  * indexed: the nearest palette entry to the colour reduced to 4 bits
///    per channel (see Quantizer).

#include "winapi/graphics/gdiplus/gdiplus_image_internal.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdi32/default_palettes.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>

namespace vitreo::winapi::gdip {

namespace {

constexpr i32 kF1 = PixelFormat1bppIndexed;
constexpr i32 kF4 = PixelFormat4bppIndexed;
constexpr i32 kF8 = PixelFormat8bppIndexed;
constexpr i32 kGray = PixelFormat16bppGrayScale;
constexpr i32 k555 = PixelFormat16bppRGB555;
constexpr i32 k565 = PixelFormat16bppRGB565;
constexpr i32 k1555 = PixelFormat16bppARGB1555;
constexpr i32 k24 = PixelFormat24bppRGB;
constexpr i32 k32 = PixelFormat32bppRGB;
constexpr i32 kArgb = PixelFormat32bppARGB;
constexpr i32 kPargb = PixelFormat32bppPARGB;
constexpr i32 k48 = PixelFormat48bppRGB;
constexpr i32 k64 = PixelFormat64bppARGB;
constexpr i32 k64p = PixelFormat64bppPARGB;
constexpr i32 kCmyk = PixelFormat32bppCMYK;

constexpr i32 kAllFormats[] = {kF1, kF4, kF8, kGray, k555, k565, k1555, k24,
                               k32, kArgb, kPargb, k48, k64, k64p, kCmyk};

bool is_ext(i32 f) { return f == k48 || f == k64 || f == k64p; }

struct LinearTables {
    u16 to_lin[256];
    u8 from_lin[8193];
};

const LinearTables& lin_tables() {
    static const LinearTables t = [] {
        LinearTables r{};
        for (int v = 0; v < 256; ++v) {
            const double c = v / 255.0;
            const double l = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
            r.to_lin[v] = static_cast<u16>(std::floor(8192.0 * l + 0.5));
        }
        for (int v = 0; v <= 8192; ++v) {
            const double l = v / 8192.0;
            const double c = l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
            r.from_lin[v] = static_cast<u8>(std::min(255.0, std::floor(255.0 * c + 0.5)));
        }
        return r;
    }();
    return t;
}

inline u32 pack(u32 a, u32 r, u32 g, u32 b) { return (a << 24) | (r << 16) | (g << 8) | b; }
inline u32 ch_a(u32 c) { return c >> 24; }
inline u32 ch_r(u32 c) { return (c >> 16) & 0xFF; }
inline u32 ch_g(u32 c) { return (c >> 8) & 0xFF; }
inline u32 ch_b(u32 c) { return c & 0xFF; }

inline u8 premul8(u32 c, u32 a) { return static_cast<u8>((c * a + 127) / 255); }
/// GDI+ unpremultiplies through a reciprocal table: (p * (65280 / a)) >> 8
/// (probe "write 000e200b": 36,12,00 at alpha 0x36 reads a9,54,00).
inline u8 unpremul8(u32 p, u32 a) { return a == 0 ? 0 : static_cast<u8>(std::min<u32>(255, (p * (65280 / a)) >> 8)); }

inline u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline void wr16(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}

/// A straight-alpha linear pixel of the extended formats. Channels are
/// signed 16-bit: GDI+ reads a WORD above 0x7FFF as negative (black / clear)
/// and saturates at 8192 (probe "write 0034400d": 0xa290 reads as 0).
struct Px64 {
    i32 b{0}, g{0}, r{0}, a{8192};
};

inline i32 s16(const u8* p) { return static_cast<i16>(rd16(p)); }

Px64 decode64(i32 f, const u8* row, i32 x) {
    Px64 p;
    if (f == k48) {
        const u8* s = row + static_cast<usize>(x) * 6;
        p.b = s16(s);
        p.g = s16(s + 2);
        p.r = s16(s + 4);
        p.a = 8192;
        return p;
    }
    const u8* s = row + static_cast<usize>(x) * 8;
    p.b = s16(s);
    p.g = s16(s + 2);
    p.r = s16(s + 4);
    p.a = s16(s + 6);
    if (f == k64p && p.a > 0 && p.a < 8192) {
        // Unpremultiply in 16-bit arithmetic (probe "write 001c400e":
        // 0xa290 premultiplied at alpha 4092 wraps to a saturated channel).
        auto un = [&](i32 c) { return static_cast<i32>(static_cast<i16>(static_cast<u16>(c * 8192 / p.a))); };
        p.b = un(p.b);
        p.g = un(p.g);
        p.r = un(p.r);
    }
    return p;
}

void encode64(i32 f, u8* row, i32 x, Px64 p) {
    auto clamp16 = [](i32 v) { return static_cast<u32>(std::clamp(v, 0, 0xFFFF)); };
    if (f == k48) {
        u8* d = row + static_cast<usize>(x) * 6;
        wr16(d, clamp16(p.b));
        wr16(d + 2, clamp16(p.g));
        wr16(d + 4, clamp16(p.r));
        return;
    }
    u8* d = row + static_cast<usize>(x) * 8;
    if (f == k64p) {
        const i32 a = std::clamp(p.a, 0, 8192);
        p.b = (std::clamp(p.b, 0, 8192) * a) >> 13;
        p.g = (std::clamp(p.g, 0, 8192) * a) >> 13;
        p.r = (std::clamp(p.r, 0, 8192) * a) >> 13;
    }
    wr16(d, clamp16(p.b));
    wr16(d + 2, clamp16(p.g));
    wr16(d + 4, clamp16(p.r));
    wr16(d + 6, clamp16(p.a));
}

Px64 argb_to_64(u32 c) {
    const LinearTables& t = lin_tables();
    return Px64{t.to_lin[ch_b(c)], t.to_lin[ch_g(c)], t.to_lin[ch_r(c)], static_cast<i32>(ch_a(c) * 8192 / 255)};
}

u32 lin_to_8(i32 v) { return v <= 0 ? 0u : linear13_to_srgb(static_cast<u32>(v)); }

u32 argb_from_64(const Px64& p) {
    const u32 a = p.a <= 0 ? 0u : p.a >= 8192 ? 255u : static_cast<u32>(p.a) * 255 / 8192;
    return pack(a, lin_to_8(p.r), lin_to_8(p.g), lin_to_8(p.b));
}

u32 expand5(u32 v) { return (v << 3) | (v >> 2); }
u32 expand6(u32 v) { return (v << 2) | (v >> 4); }

}  // namespace

// ---- Formats -------------------------------------------------------------------

bool pixel_format_valid(i32 format) {
    const i32 f = pixel_format_canonical(format);
    return std::find(std::begin(kAllFormats), std::end(kAllFormats), f) != std::end(kAllFormats);
}

bool pixel_format_indexed(i32 format) {
    const i32 f = pixel_format_canonical(format);
    return f == kF1 || f == kF4 || f == kF8;
}

bool pixel_format_opaque_storage(i32 format) {
    const i32 f = pixel_format_canonical(format);
    return f == kGray || f == kCmyk;
}

i32 pixel_format_stride(i32 width, i32 format) {
    const i64 bits = static_cast<i64>(width) * pixel_format_bits(format);
    return static_cast<i32>(((bits + 31) / 32) * 4);
}

u16 srgb_to_linear13(u8 v) { return lin_tables().to_lin[v]; }
u8 linear13_to_srgb(u32 v) { return v >= 8192 ? 255 : lin_tables().from_lin[v]; }

i32 gdip_round(float v) { return static_cast<i32>(std::floor(v + 0.5f)); }

std::vector<u32> default_palette(i32 format, u32* flags) {
    static const u32 kSystem16[16] = {0xFF000000, 0xFF800000, 0xFF008000, 0xFF808000, 0xFF000080, 0xFF800080,
                                      0xFF008080, 0xFF808080, 0xFFC0C0C0, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00,
                                      0xFF0000FF, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF};
    std::vector<u32> p;
    u32 fl = 0;
    switch (pixel_format_canonical(format)) {
    case kF1:
        p = {0xFF000000, 0xFFFFFFFF};
        fl = 2;  // PaletteFlagsGrayScale
        break;
    case kF4:
        p.assign(std::begin(kSystem16), std::end(kSystem16));
        break;
    case kF8: {
        // 16 system colours, 24 transparent black, the 6x6x6 cube
        // (red major, blue fastest), PaletteFlagsHalftone (probe "pal8_default").
        p.assign(std::begin(kSystem16), std::end(kSystem16));
        p.resize(40, 0);
        for (u32 r = 0; r < 6; ++r)
            for (u32 g = 0; g < 6; ++g)
                for (u32 b = 0; b < 6; ++b) p.push_back(pack(0xFF, r * 51, g * 51, b * 51));
        fl = 4;
        break;
    }
    default:
        break;
    }
    if (flags != nullptr) *flags = fl;
    return p;
}

u32 bitmap_flags_for(i32 format) {
    const i32 f = pixel_format_canonical(format);
    return ((f & PixelFormatAlpha) != 0 || pixel_format_indexed(f)) ? ImageFlagsHasAlpha : 0u;
}

GpImage make_bitmap(i32 width, i32 height, i32 format) {
    GpImage img;
    img.type = ImageTypeBitmap;
    img.width = width;
    img.height = height;
    img.pixel_format = format;
    img.stride = pixel_format_stride(width, format);
    img.bits.assign(static_cast<usize>(img.stride) * static_cast<usize>(height), 0);
    img.palette = default_palette(format, &img.palette_flags);
    img.flags = bitmap_flags_for(format);
    return img;
}

// ---- Rows ------------------------------------------------------------------------

i32 image_row_bytes(const GpImage& image) { return std::abs(image.stride); }

namespace {
GuestAddr guest_row_addr(const GpImage& image, i32 y) {
    return static_cast<GuestAddr>(static_cast<i64>(image.user_scan0) + static_cast<i64>(y) * image.stride);
}
}  // namespace

const u8* image_row(Marshaler& m, const GpImage& image, i32 y, std::vector<u8>& scratch) {
    const usize bytes = static_cast<usize>(image_row_bytes(image));
    if (image.user_scan0 == 0) {
        const usize off = static_cast<usize>(y) * bytes;
        if (off + bytes > image.bits.size()) return nullptr;
        return image.bits.data() + off;
    }
    scratch.resize(bytes);
    if (bytes == 0) return scratch.data();
    if (m.memory().read_bytes(guest_row_addr(image, y), scratch.data(), bytes).is_err()) return nullptr;
    return scratch.data();
}

u8* image_row_mut(Marshaler& m, GpImage& image, i32 y, std::vector<u8>& scratch) {
    const usize bytes = static_cast<usize>(image_row_bytes(image));
    if (image.user_scan0 == 0) {
        const usize off = static_cast<usize>(y) * bytes;
        if (off + bytes > image.bits.size()) return nullptr;
        return image.bits.data() + off;
    }
    scratch.resize(bytes);
    if (bytes == 0) return scratch.data();
    if (m.memory().read_bytes(guest_row_addr(image, y), scratch.data(), bytes).is_err()) return nullptr;
    return scratch.data();
}

bool image_row_commit(Marshaler& m, GpImage& image, i32 y, const std::vector<u8>& scratch) {
    if (image.user_scan0 == 0) return true;
    if (scratch.empty()) return true;
    return m.memory().write_bytes(guest_row_addr(image, y), scratch.data(), scratch.size()).is_ok();
}

// ---- Pixels ------------------------------------------------------------------------

u32 get_index(const u8* row, i32 x, i32 bpp) {
    switch (bpp) {
    case 1: return (row[x >> 3] >> (7 - (x & 7))) & 1u;
    case 4: return (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xFu;
    default: return row[x];
    }
}

void set_index(u8* row, i32 x, i32 bpp, u32 index) {
    switch (bpp) {
    case 1: {
        const u8 bit = static_cast<u8>(0x80u >> (x & 7));
        if (index & 1u) row[x >> 3] |= bit; else row[x >> 3] &= static_cast<u8>(~bit);
        break;
    }
    case 4: {
        u8& b = row[x >> 1];
        if (x & 1) b = static_cast<u8>((b & 0xF0) | (index & 0xF));
        else b = static_cast<u8>((b & 0x0F) | ((index & 0xF) << 4));
        break;
    }
    default: row[x] = static_cast<u8>(index); break;
    }
}

u32 decode_pixel(i32 format, const std::vector<u32>& palette, const u8* row, i32 x) {
    const i32 f = pixel_format_canonical(format);
    switch (f) {
    case kF1: case kF4: case kF8: {
        const u32 i = get_index(row, x, pixel_format_bits(f));
        return i < palette.size() ? palette[i] : 0xFF000000u;
    }
    case k555: {
        const u32 v = rd16(row + x * 2);
        return pack(0xFF, expand5((v >> 10) & 31), expand5((v >> 5) & 31), expand5(v & 31));
    }
    case k565: {
        const u32 v = rd16(row + x * 2);
        return pack(0xFF, expand5((v >> 11) & 31), expand6((v >> 5) & 63), expand5(v & 31));
    }
    case k1555: {
        const u32 v = rd16(row + x * 2);
        return pack((v & 0x8000) ? 0xFF : 0, expand5((v >> 10) & 31), expand5((v >> 5) & 31), expand5(v & 31));
    }
    case k24: {
        const u8* p = row + x * 3;
        return pack(0xFF, p[2], p[1], p[0]);
    }
    case k32: {
        const u8* p = row + x * 4;
        return pack(0xFF, p[2], p[1], p[0]);
    }
    case kArgb: {
        const u8* p = row + x * 4;
        return pack(p[3], p[2], p[1], p[0]);
    }
    case kPargb: {
        const u8* p = row + x * 4;
        const u32 a = p[3];
        return pack(a, unpremul8(p[2], a), unpremul8(p[1], a), unpremul8(p[0], a));
    }
    case k48: case k64: case k64p:
        return argb_from_64(decode64(f, row, x));
    default:
        return 0;
    }
}

void encode_pixel(i32 format, u8* row, i32 x, u32 c, Quantizer* q) {
    const i32 f = pixel_format_canonical(format);
    switch (f) {
    case kF1: case kF4: case kF8:
        set_index(row, x, pixel_format_bits(f), q != nullptr ? q->index(c) : 0);
        break;
    case k555:
        wr16(row + x * 2, ((ch_r(c) >> 3) << 10) | ((ch_g(c) >> 3) << 5) | (ch_b(c) >> 3));
        break;
    case k565:
        wr16(row + x * 2, ((ch_r(c) >> 3) << 11) | ((ch_g(c) >> 2) << 5) | (ch_b(c) >> 3));
        break;
    case k1555:
        wr16(row + x * 2, (ch_a(c) >= 128 ? 0x8000u : 0u) | ((ch_r(c) >> 3) << 10) | ((ch_g(c) >> 3) << 5) |
                              (ch_b(c) >> 3));
        break;
    case k24: {
        u8* p = row + x * 3;
        p[0] = static_cast<u8>(ch_b(c));
        p[1] = static_cast<u8>(ch_g(c));
        p[2] = static_cast<u8>(ch_r(c));
        break;
    }
    case k32: case kArgb: {
        u8* p = row + x * 4;
        p[0] = static_cast<u8>(ch_b(c));
        p[1] = static_cast<u8>(ch_g(c));
        p[2] = static_cast<u8>(ch_r(c));
        p[3] = static_cast<u8>(ch_a(c));
        break;
    }
    case kPargb: {
        u8* p = row + x * 4;
        const u32 a = ch_a(c);
        p[0] = premul8(ch_b(c), a);
        p[1] = premul8(ch_g(c), a);
        p[2] = premul8(ch_r(c), a);
        p[3] = static_cast<u8>(a);
        break;
    }
    case k48: case k64: case k64p:
        encode64(f, row, x, argb_to_64(c));
        break;
    default:
        break;
    }
}

bool conversion_supported(i32 sfmt, i32 dfmt) {
    const i32 s = pixel_format_canonical(sfmt), d = pixel_format_canonical(dfmt);
    if (s == d) return true;
    return !pixel_format_opaque_storage(s) && !pixel_format_opaque_storage(d);
}

bool convert_row(i32 sfmt, const std::vector<u32>& spal, const u8* src, i32 sx, i32 dfmt,
                 const std::vector<u32>& dpal, u8* dst, i32 dx, i32 count, Quantizer* q) {
    const i32 s = pixel_format_canonical(sfmt), d = pixel_format_canonical(dfmt);
    if (count <= 0) return true;
    if (s == d && (!pixel_format_indexed(s) || spal == dpal)) {
        const i32 bpp = pixel_format_bits(s);
        if (bpp >= 8) {
            const usize bytes = static_cast<usize>(bpp / 8);
            std::memmove(dst + static_cast<usize>(dx) * bytes, src + static_cast<usize>(sx) * bytes,
                         bytes * static_cast<usize>(count));
        } else {
            for (i32 i = 0; i < count; ++i) set_index(dst, dx + i, bpp, get_index(src, sx + i, bpp));
        }
        return true;
    }
    if (!conversion_supported(s, d)) return false;
    if (is_ext(s) && is_ext(d)) {
        for (i32 i = 0; i < count; ++i) encode64(d, dst, dx + i, decode64(s, src, sx + i));
        return true;
    }
    Quantizer local(dpal);
    Quantizer* qq = q != nullptr ? q : &local;
    for (i32 i = 0; i < count; ++i) encode_pixel(d, dst, dx + i, decode_pixel(s, spal, src, sx + i), qq);
    return true;
}

// ---- Quantizer -----------------------------------------------------------------------

Quantizer::Quantizer(const std::vector<u32>& palette) : m_palette(palette) { m_cache.fill(-1); }

u8 Quantizer::nearest_exact(i32 r, i32 g, i32 b) const {
    i64 best = std::numeric_limits<i64>::max();
    usize best_i = 0;
    for (usize i = 0; i < m_palette.size(); ++i) {
        const i64 dr = r - static_cast<i64>(ch_r(m_palette[i]));
        const i64 dg = g - static_cast<i64>(ch_g(m_palette[i]));
        const i64 db = b - static_cast<i64>(ch_b(m_palette[i]));
        const i64 d = dr * dr + dg * dg + db * db;
        if (d < best) {
            best = d;
            best_i = i;
        }
    }
    return static_cast<u8>(best_i);
}

u8 Quantizer::index(u32 c) {
    const u32 cell = ((ch_r(c) >> 4) << 8) | ((ch_g(c) >> 4) << 4) | (ch_b(c) >> 4);
    i16& slot = m_cache[cell];
    if (slot < 0) {
        slot = static_cast<i16>(nearest_exact(static_cast<i32>((ch_r(c) >> 4) * 17),
                                              static_cast<i32>((ch_g(c) >> 4) * 17),
                                              static_cast<i32>((ch_b(c) >> 4) * 17)));
    }
    return static_cast<u8>(slot);
}

// ---- Read / write ARGB -------------------------------------------------------------------

bool image_read_argb(Marshaler& m, const GpImage& image, i32 x, i32 y, i32 w, i32 h, std::vector<u32>& argb) {
    if (image.type != ImageTypeBitmap || pixel_format_opaque_storage(image.pixel_format)) return false;
    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > image.width || y + h > image.height) return false;
    argb.resize(static_cast<usize>(w) * static_cast<usize>(h));
    std::vector<u8> scratch;
    for (i32 j = 0; j < h; ++j) {
        const u8* row = image_row(m, image, y + j, scratch);
        if (row == nullptr) return false;
        u32* out = argb.data() + static_cast<usize>(j) * static_cast<usize>(w);
        for (i32 i = 0; i < w; ++i) out[i] = decode_pixel(image.pixel_format, image.palette, row, x + i);
    }
    return true;
}

bool image_write_argb(Marshaler& m, GpImage& image, i32 x, i32 y, i32 w, i32 h, const std::vector<u32>& argb) {
    if (image.type != ImageTypeBitmap || pixel_format_opaque_storage(image.pixel_format)) return false;
    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > image.width || y + h > image.height) return false;
    if (argb.size() < static_cast<usize>(w) * static_cast<usize>(h)) return false;
    Quantizer q(image.palette);
    std::vector<u8> scratch;
    for (i32 j = 0; j < h; ++j) {
        u8* row = image_row_mut(m, image, y + j, scratch);
        if (row == nullptr) return false;
        const u32* in = argb.data() + static_cast<usize>(j) * static_cast<usize>(w);
        for (i32 i = 0; i < w; ++i) encode_pixel(image.pixel_format, row, x + i, in[i], &q);
        if (!image_row_commit(m, image, y + j, scratch)) return false;
    }
    return true;
}

void argb_to_render(const u32* argb, usize count, u8* bgra) {
    for (usize i = 0; i < count; ++i, bgra += 4) {
        const u32 c = argb[i];
        const u32 a = ch_a(c);
        bgra[0] = premul8(ch_b(c), a);
        bgra[1] = premul8(ch_g(c), a);
        bgra[2] = premul8(ch_r(c), a);
        bgra[3] = static_cast<u8>(a);
    }
}

bool image_to_render(Marshaler& m, const GpImage& image, RenderPixels& out) {
    if (image.type != ImageTypeBitmap) return false;
    if (pixel_format_opaque_storage(image.pixel_format)) {
        log_unimplemented("RenderScope", "drawing a 16bppGrayScale / 32bppCMYK bitmap (GDI+ cannot either)");
        return false;
    }
    out.width = image.width;
    out.height = image.height;
    out.bgra.assign(static_cast<usize>(image.width) * static_cast<usize>(image.height) * 4u, 0);
    std::vector<u8> scratch;
    const bool pargb = pixel_format_canonical(image.pixel_format) == kPargb;
    for (i32 y = 0; y < image.height; ++y) {
        const u8* row = image_row(m, image, y, scratch);
        if (row == nullptr) return false;
        u8* o = out.bgra.data() + static_cast<usize>(y) * static_cast<usize>(image.width) * 4u;
        if (pargb) {
            std::memcpy(o, row, static_cast<usize>(image.width) * 4u);
            continue;
        }
        for (i32 x = 0; x < image.width; ++x) {
            const u32 c = decode_pixel(image.pixel_format, image.palette, row, x);
            argb_to_render(&c, 1, o + static_cast<usize>(x) * 4u);
        }
    }
    return true;
}

bool render_to_image(Marshaler& m, GpImage& image, const RenderPixels& px) {
    if (image.type != ImageTypeBitmap || pixel_format_opaque_storage(image.pixel_format)) return false;
    if (px.width != image.width || px.height != image.height) return false;
    const bool pargb = pixel_format_canonical(image.pixel_format) == kPargb;
    Quantizer q(image.palette);
    std::vector<u8> scratch;
    for (i32 y = 0; y < image.height; ++y) {
        u8* row = image_row_mut(m, image, y, scratch);
        if (row == nullptr) return false;
        const u8* s = px.bgra.data() + static_cast<usize>(y) * static_cast<usize>(image.width) * 4u;
        if (pargb) {
            std::memcpy(row, s, static_cast<usize>(image.width) * 4u);
        } else {
            for (i32 x = 0; x < image.width; ++x, s += 4) {
                const u32 a = s[3];
                // Unpremultiplying truncates (probe "none": 80ff4020 drawn
                // SourceCopy into ARGB reads 80ff3f1f).
                const u32 c = pack(a, unpremul8(s[2], a), unpremul8(s[1], a), unpremul8(s[0], a));
                encode_pixel(image.pixel_format, row, x, c, &q);
            }
        }
        if (!image_row_commit(m, image, y, scratch)) return false;
    }
    return true;
}

// ---- Palettes ---------------------------------------------------------------------------

namespace {

void add_cube(std::vector<u32>& out, const std::vector<u32>& rl, const std::vector<u32>& gl,
              const std::vector<u32>& bl) {
    for (u32 r : rl)
        for (u32 g : gl)
            for (u32 b : bl) out.push_back(pack(0xFF, r, g, b));
}

std::vector<u32> levels(u32 n) {
    std::vector<u32> l;
    for (u32 i = 0; i < n; ++i) l.push_back(i * 255 / (n - 1));
    return l;
}

void add_extras(std::vector<u32>& out) {
    static const u32 kExtras[8] = {0xFFC0C0C0, 0xFF808080, 0xFF800000, 0xFF008000,
                                   0xFF000080, 0xFF808000, 0xFF800080, 0xFF008080};
    out.insert(out.end(), std::begin(kExtras), std::end(kExtras));
}

}  // namespace

bool fixed_palette(i32 type, std::vector<u32>& out) {
    out.clear();
    switch (type) {
    case 2: out = {0xFF000000, 0xFFFFFFFF}; return true;                       // FixedBW
    case 3: add_cube(out, levels(2), levels(2), levels(2)); add_extras(out); return true;   // Halftone8
    case 4: add_cube(out, levels(3), levels(3), levels(3)); add_extras(out); return true;   // Halftone27
    case 5: add_cube(out, levels(4), levels(4), levels(4)); add_extras(out); return true;   // Halftone64
    case 6: add_cube(out, levels(5), levels(5), levels(5)); add_extras(out); return true;   // Halftone125
    case 7: add_cube(out, levels(6), levels(6), levels(6)); add_extras(out); return true;   // Halftone216
    case 8: add_cube(out, levels(6), levels(7), levels(6)); return true;                    // Halftone252
    case 9: {                                                                               // Halftone256
        // Eight red/green levels at a step of 36.4 (floor), four blue levels.
        std::vector<u32> rg;
        for (u32 i = 0; i < 8; ++i) rg.push_back(i * 364 / 10);
        add_cube(out, rg, rg, levels(4));
        return true;
    }
    case 10:  // not in the SDK enum; GDI+ answers the 16 VGA colours (probe "initpal t10")
        out = {0xFF000000, 0xFFFFFFFF, 0xFFC0C0C0, 0xFF808080, 0xFF800000, 0xFFFF0000, 0xFF008000, 0xFF00FF00,
               0xFF000080, 0xFF0000FF, 0xFF808000, 0xFFFFFF00, 0xFF800080, 0xFFFF00FF, 0xFF008080, 0xFF00FFFF};
        return true;
    default:
        return false;
    }
}

const std::array<u32, 256>& halftone_palette_entries() {
    // GDI's 8bpp system table (gdi32 default_palettes.hpp, documented in
    // .docs/contracts/gdi32.md "GetDIBits colour tables"): the 20 static
    // colours (flags 0) at the ends, and between them 236 PC_NOCOLLAPSE (4)
    // entries, the 6x6x6 cube and a 20-step gray ramp. Windows' four extra
    // static colours and its 236-entry order are not copied (contract note
    // in .docs/contracts/gdiplus/image.md).
    static const std::array<u32, 256> kTable = [] {
        std::array<u32, 256> t{};
        for (usize i = 0; i < t.size(); ++i) {
            const bool is_static = i < 10 || i >= 246;
            t[i] = gdi::kGdiDefaultPalette8[i] | (is_static ? 0u : 0x04000000u);
        }
        return t;
    }();
    return kTable;
}

std::vector<u32> optimal_palette(const std::vector<u32>& argb, i32 colors) {
    // Distinct colours (alpha ignored) in first-seen order.
    std::vector<u32> distinct;
    std::map<u32, usize> counts;
    for (u32 c : argb) {
        const u32 rgb = c & 0xFFFFFF;
        auto [it, inserted] = counts.emplace(rgb, 0);
        if (inserted) distinct.push_back(rgb);
        ++it->second;
    }
    std::vector<u32> out;
    if (static_cast<i32>(distinct.size()) <= colors) {
        for (u32 c : distinct) out.push_back(0xFF000000u | c);
        return out;
    }
    // Median cut over the weighted colour set: split the box with the
    // largest channel range at its weighted median until @p colors boxes;
    // each entry is its box's weighted mean.
    struct Box { std::vector<std::pair<u32, usize>> px; };
    std::vector<Box> boxes(1);
    for (u32 c : distinct) boxes[0].px.emplace_back(c, counts[c]);
    auto channel = [](u32 c, int ch) { return static_cast<i32>((c >> (16 - 8 * ch)) & 0xFF); };
    while (static_cast<i32>(boxes.size()) < colors) {
        i32 best_box = -1, best_ch = 0, best_range = 0;
        for (usize bi = 0; bi < boxes.size(); ++bi) {
            if (boxes[bi].px.size() < 2) continue;
            for (int ch = 0; ch < 3; ++ch) {
                i32 lo = 255, hi = 0;
                for (auto& [c, n] : boxes[bi].px) {
                    lo = std::min(lo, channel(c, ch));
                    hi = std::max(hi, channel(c, ch));
                }
                if (hi - lo > best_range) {
                    best_range = hi - lo;
                    best_box = static_cast<i32>(bi);
                    best_ch = ch;
                }
            }
        }
        if (best_box < 0) break;
        Box& b = boxes[static_cast<usize>(best_box)];
        std::sort(b.px.begin(), b.px.end(), [&](const auto& l, const auto& r) {
            return channel(l.first, best_ch) < channel(r.first, best_ch);
        });
        usize total = 0;
        for (auto& p : b.px) total += p.second;
        usize acc = 0, cut = 1;
        for (usize i = 0; i + 1 < b.px.size(); ++i) {
            acc += b.px[i].second;
            cut = i + 1;
            if (acc * 2 >= total) break;
        }
        Box nb;
        nb.px.assign(b.px.begin() + static_cast<std::ptrdiff_t>(cut), b.px.end());
        b.px.resize(cut);
        boxes.push_back(std::move(nb));
    }
    for (const Box& b : boxes) {
        u64 r = 0, g = 0, bl = 0, n = 0;
        for (auto& [c, w] : b.px) {
            r += static_cast<u64>(channel(c, 0)) * w;
            g += static_cast<u64>(channel(c, 1)) * w;
            bl += static_cast<u64>(channel(c, 2)) * w;
            n += w;
        }
        if (n == 0) continue;
        out.push_back(pack(0xFF, static_cast<u32>((r + n / 2) / n), static_cast<u32>((g + n / 2) / n),
                           static_cast<u32>((bl + n / 2) / n)));
    }
    return out;
}

// ---- Dithering --------------------------------------------------------------------------

namespace {

/// Threshold matrices, values 0..n*n-1. Bayer for the ordered dithers;
/// clustered spirals for the spiral ones.
struct DitherMatrix {
    i32 n{0};
    std::vector<i32> v;
};

DitherMatrix bayer(i32 n) {
    DitherMatrix m;
    m.n = 1;
    m.v = {0};
    while (m.n < n) {
        DitherMatrix next;
        next.n = m.n * 2;
        next.v.assign(static_cast<usize>(next.n * next.n), 0);
        const i32 add[4] = {0, 2, 3, 1};
        for (i32 q = 0; q < 4; ++q) {
            const i32 ox = (q & 1) * m.n, oy = (q >> 1) * m.n;
            for (i32 y = 0; y < m.n; ++y)
                for (i32 x = 0; x < m.n; ++x)
                    next.v[static_cast<usize>((oy + y) * next.n + ox + x)] =
                        4 * m.v[static_cast<usize>(y * m.n + x)] + add[q];
        }
        m = std::move(next);
    }
    return m;
}

DitherMatrix spiral(i32 n, bool dual) {
    // Clustered dot: thresholds grow outward from the cell centre in a
    // square spiral. The dual spiral interleaves two half-weight clusters
    // on the diagonal.
    DitherMatrix m;
    m.n = n;
    m.v.assign(static_cast<usize>(n * n), 0);
    std::vector<std::pair<float, i32>> order;
    for (i32 y = 0; y < n; ++y) {
        for (i32 x = 0; x < n; ++x) {
            float cx = (n - 1) / 2.0f, cy = (n - 1) / 2.0f;
            float dx = x - cx, dy = y - cy;
            if (dual) {
                const bool second = (x < n / 2) != (y < n / 2);
                const float qx = (x % (n / 2)) - (n / 2 - 1) / 2.0f, qy = (y % (n / 2)) - (n / 2 - 1) / 2.0f;
                dx = qx;
                dy = qy;
                order.emplace_back(std::max(std::fabs(dx), std::fabs(dy)) * 4 + std::atan2(dy, dx) / 6.3f +
                                       (second ? 0.5f : 0.0f) + 4.0f,
                                   y * n + x);
                continue;
            }
            order.emplace_back(std::max(std::fabs(dx), std::fabs(dy)) * 4 + std::atan2(dy, dx) / 6.3f + 4.0f,
                               y * n + x);
        }
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& l, const auto& r) { return l.first < r.first; });
    for (usize i = 0; i < order.size(); ++i) m.v[static_cast<usize>(order[i].second)] = static_cast<i32>(i);
    return m;
}

DitherMatrix matrix_for(i32 dither) {
    switch (dither) {
    case 2: return bayer(4);
    case 3: return bayer(8);
    case 4: return bayer(16);
    case 5: return spiral(4, false);
    case 6: return spiral(8, false);
    case 7: return spiral(4, true);
    case 8: return spiral(8, true);
    default: return {};
    }
}

/// Level spacing of a fixed halftone palette per channel (R, G, B).
void palette_steps(i32 palette_type, float steps[3]) {
    float r = 255, g = 255, b = 255;
    switch (palette_type) {
    case 2: r = g = b = 255; break;
    case 3: r = g = b = 255; break;
    case 4: r = g = b = 127.5f; break;
    case 5: r = g = b = 85; break;
    case 6: r = g = b = 63.75f; break;
    case 7: r = g = b = 51; break;
    case 8: r = 51; g = 42.5f; b = 51; break;
    case 9: r = g = 36.4f; b = 85; break;
    default: r = g = b = 51; break;
    }
    steps[0] = r;
    steps[1] = g;
    steps[2] = b;
}

inline u32 clamp8(float v) { return static_cast<u32>(std::clamp(static_cast<i32>(std::floor(v + 0.5f)), 0, 255)); }

}  // namespace

void dither_into(const std::vector<u32>& argb, i32 width, i32 height, i32 format, i32 dither, i32 palette_type,
                 const std::vector<u32>& palette, i32 alpha_threshold, std::vector<u8>& bits) {
    const i32 f = pixel_format_canonical(format);
    const i32 stride = pixel_format_stride(width, f);
    bits.assign(static_cast<usize>(stride) * static_cast<usize>(height), 0);
    const DitherMatrix dm = matrix_for(dither);
    if (!pixel_format_indexed(f)) {
        // Only Ordered4x4 dithers a 16bpp target (GdiPlusPixelFormats.h
        // note; probe "dither2 tgt2").
        const bool ordered = dither == 2 && (f == k555 || f == k565 || f == k1555);
        for (i32 y = 0; y < height; ++y) {
            u8* row = bits.data() + static_cast<usize>(y) * static_cast<usize>(stride);
            for (i32 x = 0; x < width; ++x) {
                u32 c = argb[static_cast<usize>(y) * static_cast<usize>(width) + static_cast<usize>(x)];
                if (ordered) {
                    const float t = (dm.v[static_cast<usize>((y % dm.n) * dm.n + (x % dm.n))] + 0.5f) /
                                        static_cast<float>(dm.n * dm.n) - 0.5f;
                    const float gs = f == k565 ? 4.0f : 8.0f;
                    c = pack(ch_a(c), clamp8(ch_r(c) + t * 8.0f), clamp8(ch_g(c) + t * gs), clamp8(ch_b(c) + t * 8.0f));
                }
                encode_pixel(f, row, x, c, nullptr);
            }
        }
        return;
    }
    const i32 bpp = pixel_format_bits(f);
    Quantizer q(palette);
    i32 transparent = -1;
    for (usize i = 0; i < palette.size(); ++i) {
        if (ch_a(palette[i]) == 0) { transparent = static_cast<i32>(i); break; }
    }
    float steps[3];
    palette_steps(palette_type, steps);
    // Floyd-Steinberg error buffers (current and next row), per channel.
    std::vector<float> err_cur(static_cast<usize>(width + 2) * 3, 0.0f), err_next(err_cur.size(), 0.0f);
    for (i32 y = 0; y < height; ++y) {
        u8* row = bits.data() + static_cast<usize>(y) * static_cast<usize>(stride);
        std::fill(err_next.begin(), err_next.end(), 0.0f);
        for (i32 x = 0; x < width; ++x) {
            const u32 c = argb[static_cast<usize>(y) * static_cast<usize>(width) + static_cast<usize>(x)];
            u32 idx = 0;
            if (static_cast<i32>(ch_a(c)) < alpha_threshold) {
                idx = transparent >= 0 ? static_cast<u32>(transparent) : 0u;
            } else if (dither == 9) {
                float rgb[3];
                for (int k = 0; k < 3; ++k)
                    rgb[k] = static_cast<float>((c >> (16 - 8 * k)) & 0xFF) +
                             err_cur[static_cast<usize>(x + 1) * 3 + static_cast<usize>(k)];
                idx = q.nearest_exact(static_cast<i32>(clamp8(rgb[0])), static_cast<i32>(clamp8(rgb[1])),
                                      static_cast<i32>(clamp8(rgb[2])));
                const u32 p = palette.empty() ? 0 : palette[idx];
                for (int k = 0; k < 3; ++k) {
                    const float e = rgb[k] - static_cast<float>((p >> (16 - 8 * k)) & 0xFF);
                    err_cur[static_cast<usize>(x + 2) * 3 + static_cast<usize>(k)] += e * 7 / 16;
                    err_next[static_cast<usize>(x) * 3 + static_cast<usize>(k)] += e * 3 / 16;
                    err_next[static_cast<usize>(x + 1) * 3 + static_cast<usize>(k)] += e * 5 / 16;
                    err_next[static_cast<usize>(x + 2) * 3 + static_cast<usize>(k)] += e * 1 / 16;
                }
            } else if (dm.n > 0) {
                const float t = (dm.v[static_cast<usize>((y % dm.n) * dm.n + (x % dm.n))] + 0.5f) /
                                    static_cast<float>(dm.n * dm.n) - 0.5f;
                idx = q.nearest_exact(static_cast<i32>(clamp8(ch_r(c) + t * steps[0])),
                                      static_cast<i32>(clamp8(ch_g(c) + t * steps[1])),
                                      static_cast<i32>(clamp8(ch_b(c) + t * steps[2])));
            } else {
                idx = q.index(c);
            }
            set_index(row, x, bpp, idx);
        }
        std::swap(err_cur, err_next);
    }
}

// ---- Rotate / flip ------------------------------------------------------------------------

bool rotate_flip_image(Marshaler& m, GpImage& image, i32 type) {
    const i32 w = image.width, h = image.height;
    const bool swap = (type & 1) != 0;
    const i32 nw = swap ? h : w, nh = swap ? w : h;
    const i32 bpp = pixel_format_bits(image.pixel_format);
    const i32 nstride = pixel_format_stride(nw, image.pixel_format);
    std::vector<u8> out(static_cast<usize>(nstride) * static_cast<usize>(nh), 0);
    std::vector<std::vector<u8>> rows(static_cast<usize>(h));
    std::vector<u8> scratch;
    for (i32 y = 0; y < h; ++y) {
        const u8* r = image_row(m, image, y, scratch);
        if (r == nullptr) return false;
        rows[static_cast<usize>(y)].assign(r, r + image_row_bytes(image));
    }
    // RotateFlipType: rotation (clockwise) by (type & 3) * 90, then a
    // horizontal flip when bit 2 is set (GdiPlusImaging.h; probe "rotate").
    const i32 rot = type & 3;
    const bool flip = (type & 4) != 0;
    const usize bytes = bpp >= 8 ? static_cast<usize>(bpp / 8) : 0;
    for (i32 ny = 0; ny < nh; ++ny) {
        u8* drow = out.data() + static_cast<usize>(ny) * static_cast<usize>(nstride);
        for (i32 nx = 0; nx < nw; ++nx) {
            const i32 fx = flip ? nw - 1 - nx : nx;  // undo the flip first
            i32 sx = 0, sy = 0;
            switch (rot) {
            case 0: sx = fx; sy = ny; break;
            case 1: sx = ny; sy = h - 1 - fx; break;
            case 2: sx = w - 1 - fx; sy = h - 1 - ny; break;
            default: sx = w - 1 - ny; sy = fx; break;
            }
            const u8* srow = rows[static_cast<usize>(sy)].data();
            if (bytes > 0) {
                std::memcpy(drow + static_cast<usize>(nx) * bytes, srow + static_cast<usize>(sx) * bytes, bytes);
            } else {
                set_index(drow, nx, bpp, get_index(srow, sx, bpp));
            }
        }
    }
    image.bits = std::move(out);
    image.stride = nstride;
    image.user_scan0 = 0;
    image.width = nw;
    image.height = nh;
    if (swap) std::swap(image.dpi_x, image.dpi_y);
    return true;
}

}  // namespace vitreo::winapi::gdip
