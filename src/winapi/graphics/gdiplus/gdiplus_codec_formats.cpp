// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_codec_formats.cpp
/// @brief GDI+ codec area: BMP/PNG/GIF/TIFF/EXIF/JPEG-marker parsing and the
/// BMP/PNG/GIF/TIFF/JPEG(EXIF) encoders. See gdiplus_codec_formats.hpp.

#include "winapi/graphics/gdiplus/gdiplus_codec_formats.hpp"
#include "graphics/common/png_decode.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include <zlib.h>

namespace vitreo::winapi::gdip::fmt {

namespace {

// ---- byte helpers ----------------------------------------------------------------

u16 le16(std::span<const u8> b, usize at) { return at + 2 <= b.size() ? static_cast<u16>(b[at] | (b[at + 1] << 8)) : 0; }
u32 le32(std::span<const u8> b, usize at) {
    return at + 4 <= b.size() ? static_cast<u32>(b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (u32{b[at + 3]} << 24)) : 0;
}
u16 be16(std::span<const u8> b, usize at) { return at + 2 <= b.size() ? static_cast<u16>((b[at] << 8) | b[at + 1]) : 0; }
u32 be32(std::span<const u8> b, usize at) {
    return at + 4 <= b.size() ? static_cast<u32>((u32{b[at]} << 24) | (b[at + 1] << 16) | (b[at + 2] << 8) | b[at + 3]) : 0;
}
void put16(std::vector<u8>& v, u16 x) { v.push_back(static_cast<u8>(x)); v.push_back(static_cast<u8>(x >> 8)); }
void put32(std::vector<u8>& v, u32 x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i))); }
void putbe32(std::vector<u8>& v, u32 x) { for (int i = 3; i >= 0; --i) v.push_back(static_cast<u8>(x >> (8 * i))); }
void set32(std::vector<u8>& v, usize at, u32 x) { for (int i = 0; i < 4; ++i) v[at + i] = static_cast<u8>(x >> (8 * i)); }

std::vector<u8> u16v(u16 x) { return {static_cast<u8>(x), static_cast<u8>(x >> 8)}; }
std::vector<u8> u32v(u32 x) { std::vector<u8> v; put32(v, x); return v; }
std::vector<u8> rational(u32 n, u32 d) { std::vector<u8> v; put32(v, n); put32(v, d); return v; }
std::vector<u8> ascii(std::string_view s) { std::vector<u8> v(s.begin(), s.end()); v.push_back(0); return v; }

constexpr u32 kAlphaMask = 0xFF000000u;

// Image flags (GdiPlusImaging.h ImageFlags).
constexpr u32 kFlagHasAlpha = 0x0002, kFlagRGB = 0x0010, kFlagCMYK = 0x0020, kFlagGray = 0x0040,
              kFlagYCbCr = 0x0080, kFlagRealDpi = 0x1000, kFlagRealSize = 0x2000, kFlagReadOnly = 0x10000;
// The flags every decoded raster image carries (probe: 0x12010 for RGB).
constexpr u32 kDecodedBase = kFlagReadOnly | kFlagRealSize;

bool has_alpha_format(i32 f) { return (f & PixelFormatAlpha) != 0; }

void finish_frame(DecodedFrame& f, u32 colorspace, bool real_dpi) {
    f.flags = kDecodedBase | colorspace | (real_dpi ? kFlagRealDpi : 0) |
              (has_alpha_format(f.pixel_format) ? kFlagHasAlpha : 0);
}

std::vector<u32> gray_palette(u32 n) {
    std::vector<u32> p(n);
    for (u32 i = 0; i < n; ++i) {
        const u32 g = n == 1 ? 0 : i * 255 / (n - 1);
        p[i] = kAlphaMask | (g << 16) | (g << 8) | g;
    }
    return p;
}

void frame_from_argb(DecodedFrame& f, const std::vector<u32>& argb, i32 w, i32 h, i32 format) {
    argb_to_frame(argb, w, h, format, f.palette, f);
}

// ---- BMP -----------------------------------------------------------------------------

u32 mask_shift(u32 m) { u32 s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; ++s; } return s; }
u32 mask_bits(u32 m) { u32 n = 0; m >>= mask_shift(m); while (m & 1) { m >>= 1; ++n; } return n; }
u32 expand(u32 v, u32 bits) {
    if (bits == 0) return 0;
    if (bits >= 8) return v >> (bits - 8);
    u32 r = v << (8 - bits);
    for (u32 s = bits; s < 8; s += bits) r |= v << (8 - bits) >> s;
    return r & 0xFF;
}

Status decode_bmp(std::span<const u8> b, Decoded& out) {
    if (b.size() < 26) return Status::OutOfMemory;
    const u32 off = le32(b, 10);
    const u32 hsize = le32(b, 14);
    i32 w, h;
    u16 bpp;
    u32 comp = 0, clr_used = 0, ppm_x = 0, ppm_y = 0;
    u32 masks[4] = {0, 0, 0, 0};
    usize pal_at, pal_entry;
    if (hsize == 12) {
        w = le16(b, 18); h = static_cast<i16>(le16(b, 20)); bpp = le16(b, 24);
        pal_at = 26; pal_entry = 3;
    } else if (hsize >= 40) {
        w = static_cast<i32>(le32(b, 18)); h = static_cast<i32>(le32(b, 22)); bpp = le16(b, 28);
        comp = le32(b, 30); ppm_x = le32(b, 38); ppm_y = le32(b, 42); clr_used = le32(b, 46);
        pal_at = 14 + hsize; pal_entry = 4;
        if (comp == 3 || comp == 6) {
            const usize m = hsize >= 52 ? 54 : pal_at;
            masks[0] = le32(b, m); masks[1] = le32(b, m + 4); masks[2] = le32(b, m + 8);
            if (hsize >= 56 || comp == 6) masks[3] = le32(b, m + 12);
            if (hsize < 52) pal_at += comp == 6 ? 16 : 12;
        } else if (hsize >= 56 && bpp == 32) {
            masks[3] = le32(b, 66);
        }
    } else {
        return Status::OutOfMemory;
    }
    const bool top_down = h < 0;
    h = std::abs(h);
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535) return Status::OutOfMemory;
    DecodedFrame& f = out.frame;
    std::vector<u32> argb(static_cast<usize>(w) * static_cast<usize>(h), kAlphaMask);
    i32 format = PixelFormat32bppRGB;
    std::vector<u32> palette;
    if (bpp <= 8) {
        const u32 n = clr_used ? std::min<u32>(clr_used, 1u << bpp) : (1u << bpp);
        for (u32 i = 0; i < n; ++i) {
            const usize at = pal_at + i * pal_entry;
            palette.push_back(kAlphaMask | (u32{b.size() > at + 2 ? b[at + 2] : u8{0}} << 16) |
                              (u32{b.size() > at + 1 ? b[at + 1] : u8{0}} << 8) | (b.size() > at ? b[at] : 0));
        }
    }
    auto px = [&](i32 x, i32 y) -> u32& { return argb[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)]; };
    auto pal = [&](u32 i) { return i < palette.size() ? palette[i] : kAlphaMask; };
    if (comp == 1 || comp == 2) {
        // RLE8 / RLE4 (Learn: Bitmap Compression). GDI+ decodes to 32bppRGB.
        std::vector<i32> idx(static_cast<usize>(w) * static_cast<usize>(h), -1);
        i32 x = 0, y = 0;
        usize p = off;
        while (p + 1 < b.size() && y < h) {
            const u8 n = b[p], c = b[p + 1];
            p += 2;
            if (n) {
                for (u32 i = 0; i < n; ++i, ++x)
                    if (x < w) idx[static_cast<usize>(y) * w + x] = comp == 1 ? c : ((i & 1) ? (c & 15) : (c >> 4));
            } else if (c == 0) { x = 0; ++y; }
            else if (c == 1) break;
            else if (c == 2) { if (p + 1 >= b.size()) break; x += b[p]; y += b[p + 1]; p += 2; }
            else {
                const usize bytes = comp == 1 ? c : (c + 1u) / 2u;
                for (u32 i = 0; i < c; ++i, ++x) {
                    const u8 v = p + (comp == 1 ? i : i / 2) < b.size() ? b[p + (comp == 1 ? i : i / 2)] : 0;
                    if (x < w) idx[static_cast<usize>(y) * w + x] = comp == 1 ? v : ((i & 1) ? (v & 15) : (v >> 4));
                }
                p += (bytes + 1) & ~usize{1};
            }
        }
        for (i32 yy = 0; yy < h; ++yy)
            for (i32 xx = 0; xx < w; ++xx) {
                const i32 i = idx[static_cast<usize>(yy) * w + xx];
                px(xx, h - 1 - yy) = i < 0 ? pal(0) : pal(static_cast<u32>(i));
            }
        palette.clear();
        format = PixelFormat32bppRGB;
    } else if (comp == 0 || comp == 3 || comp == 6) {
        const usize stride = ((static_cast<usize>(w) * bpp + 31) / 32) * 4;
        if (off + stride * static_cast<usize>(h) > b.size()) return Status::OutOfMemory;
        if (bpp == 16 && comp == 0) { masks[0] = 0x7C00; masks[1] = 0x3E0; masks[2] = 0x1F; }
        if (bpp == 32 && comp == 0) { masks[0] = 0xFF0000; masks[1] = 0xFF00; masks[2] = 0xFF; }
        for (i32 y = 0; y < h; ++y) {
            const u8* row = b.data() + off + stride * static_cast<usize>(top_down ? y : h - 1 - y);
            for (i32 x = 0; x < w; ++x) {
                u32 c;
                switch (bpp) {
                    case 1: c = pal((row[x >> 3] >> (7 - (x & 7))) & 1); break;
                    case 4: c = pal((x & 1) ? (row[x >> 1] & 15) : (row[x >> 1] >> 4)); break;
                    case 8: c = pal(row[x]); break;
                    case 24: c = kAlphaMask | (u32{row[x * 3 + 2]} << 16) | (u32{row[x * 3 + 1]} << 8) | row[x * 3]; break;
                    case 16:
                    case 32: {
                        const u32 v = bpp == 16 ? static_cast<u32>(row[x * 2] | (row[x * 2 + 1] << 8))
                                                : static_cast<u32>(row[x * 4] | (row[x * 4 + 1] << 8) | (row[x * 4 + 2] << 16) | (u32{row[x * 4 + 3]} << 24));
                        auto ch = [&](u32 m) { return expand((v & m) >> mask_shift(m), mask_bits(m)); };
                        const u32 a = masks[3] ? ch(masks[3]) : 0xFF;
                        c = (a << 24) | (ch(masks[0]) << 16) | (ch(masks[1]) << 8) | ch(masks[2]);
                        break;
                    }
                    case 64: c = 0; break;
                    default: return Status::OutOfMemory;
                }
                px(x, y) = c;
            }
        }
        if (bpp == 64) {
            // GDI+'s own 64bpp BMP: 64bppARGB rows stored verbatim (probe r2.savefmt 0x34400d).
            f.width = w; f.height = h; f.pixel_format = PixelFormat64bppARGB;
            f.stride = stride_for(w, f.pixel_format);
            f.bits.assign(static_cast<usize>(f.stride) * h, 0);
            for (i32 y = 0; y < h; ++y)
                std::memcpy(f.bits.data() + static_cast<usize>(y) * f.stride,
                            b.data() + off + stride * static_cast<usize>(top_down ? y : h - 1 - y), static_cast<usize>(w) * 8);
            format = -1;
        } else if (bpp <= 8) {
            format = bpp == 1 ? PixelFormat1bppIndexed : bpp == 4 ? PixelFormat4bppIndexed : PixelFormat8bppIndexed;
        } else if (bpp == 24) {
            format = PixelFormat24bppRGB;
        } else {
            // 16bpp (any masks) and 32bpp without an alpha mask: 32bppRGB;
            // an alpha mask (BITMAPV4/V5): 32bppARGB (probe rgb555/rgb565/v5alpha).
            format = masks[3] && bpp == 32 ? PixelFormat32bppARGB : PixelFormat32bppRGB;
        }
    } else {
        return Status::OutOfMemory;   // BI_JPEG / BI_PNG inside a .bmp file
    }
    if (format >= 0) {
        f.palette = palette;
        frame_from_argb(f, argb, w, h, format);
    }
    const bool real = ppm_x != 0 && ppm_y != 0;
    if (real) { f.dpi_x = ppm_x * 0.0254f; f.dpi_y = ppm_y * 0.0254f; }
    finish_frame(f, kFlagRGB, real);
    return Status::Ok;
}

// ---- TIFF IFDs (TIFF and EXIF) -----------------------------------------------------------

struct Tiff {
    std::span<const u8> b;
    bool le{true};
    u16 r16(usize at) const { return le ? le16(b, at) : be16(b, at); }
    u32 r32(usize at) const { return le ? le32(b, at) : be32(b, at); }
};

u32 type_size(u16 t) {
    switch (t) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

struct IfdEntry { u16 tag; u16 type; u32 count; std::vector<u8> value; /* little-endian */ };

/// Read one IFD at @p at; values converted to little-endian. Next offset in @p next.
std::vector<IfdEntry> read_ifd(const Tiff& t, usize at, u32* next) {
    std::vector<IfdEntry> out;
    if (at + 2 > t.b.size()) return out;
    const u16 n = t.r16(at);
    for (u16 i = 0; i < n; ++i) {
        const usize e = at + 2 + i * 12u;
        if (e + 12 > t.b.size()) break;
        IfdEntry entry{t.r16(e), t.r16(e + 2), t.r32(e + 4), {}};
        const u32 ts = type_size(entry.type);
        const u64 bytes = static_cast<u64>(ts) * entry.count;
        if (ts == 0 || bytes > t.b.size()) continue;
        const usize src = bytes <= 4 ? e + 8 : t.r32(e + 8);
        if (src + bytes > t.b.size()) continue;
        entry.value.assign(t.b.begin() + static_cast<std::ptrdiff_t>(src), t.b.begin() + static_cast<std::ptrdiff_t>(src + bytes));
        if (!t.le && ts > 1) {
            const u32 unit = (entry.type == 5 || entry.type == 10) ? 4 : ts;
            for (usize k = 0; k + unit <= entry.value.size(); k += unit) std::reverse(entry.value.begin() + k, entry.value.begin() + k + unit);
        }
        out.push_back(std::move(entry));
    }
    if (next) *next = at + 2 + n * 12u + 4 <= t.b.size() ? t.r32(at + 2 + n * 12u) : 0;
    // GDI+ reports an IFD's tags in tag order (probe exif2.jpg 0x9204 before 0x9286).
    std::stable_sort(out.begin(), out.end(), [](const IfdEntry& a, const IfdEntry& b) { return a.tag < b.tag; });
    return out;
}

u32 entry_uint(const IfdEntry& e, u32 index = 0) {
    const u32 ts = type_size(e.type);
    const usize at = index * ts;
    if (at + ts > e.value.size()) return 0;
    if (ts == 1) return e.value[at];
    if (ts == 2) return static_cast<u32>(e.value[at] | (e.value[at + 1] << 8));
    return static_cast<u32>(e.value[at] | (e.value[at + 1] << 8) | (e.value[at + 2] << 16) | (u32{e.value[at + 3]} << 24));
}
const IfdEntry* find(const std::vector<IfdEntry>& ifd, u16 tag) {
    for (const auto& e : ifd) if (e.tag == tag) return &e;
    return nullptr;
}

Property to_property(const IfdEntry& e) { return Property{e.tag, e.type, e.value}; }

/// EXIF UserComment: GDI+ reports the ASCII/UNICODE character-code forms as
/// a NUL-terminated UTF-16 string (probe exif2.jpg 0x9286).
Property user_comment(const IfdEntry& e) {
    Property p = to_property(e);
    if (e.value.size() >= 8 && std::memcmp(e.value.data(), "ASCII\0\0\0", 8) == 0) {
        p.value.clear();
        for (usize i = 8; i < e.value.size() && e.value[i] != 0; ++i) { p.value.push_back(e.value[i]); p.value.push_back(0); }
        p.value.push_back(0); p.value.push_back(0);
    } else if (e.value.size() >= 8 && std::memcmp(e.value.data(), "UNICODE\0", 8) == 0) {
        p.value.assign(e.value.begin() + 8, e.value.end());
        p.value.push_back(0); p.value.push_back(0);
    }
    return p;
}

/// IFD1 (thumbnail) tag -> PropertyTagThumbnail* (GdiPlusImaging.h names).
u32 thumbnail_tag(u16 tag) {
    static const std::pair<u16, u16> map[] = {
        {0x100, 0x5020}, {0x101, 0x5021}, {0x102, 0x5022}, {0x103, 0x5023}, {0x106, 0x5024}, {0x10E, 0x5025},
        {0x10F, 0x5026}, {0x110, 0x5027}, {0x111, 0x5028}, {0x112, 0x5029}, {0x115, 0x502A}, {0x116, 0x502B},
        {0x117, 0x502C}, {0x11A, 0x502D}, {0x11B, 0x502E}, {0x11C, 0x502F}, {0x128, 0x5030}, {0x12D, 0x5031},
        {0x131, 0x5032}, {0x132, 0x5033}, {0x13B, 0x5034}, {0x13E, 0x5035}, {0x13F, 0x5036}, {0x211, 0x5037},
        {0x212, 0x5038}, {0x213, 0x5039}, {0x214, 0x503A}, {0x8298, 0x503B}};
    for (auto [t, p] : map) if (t == tag) return p;
    return tag;
}
constexpr u16 kExifIfd = 0x8769, kGpsIfd = 0x8825, kInteropIfd = 0xA005;

/// The EXIF property items of a TIFF structure (an Exif APP1 body or a TIFF
/// file): IFD0 without the pointer tags, then the Exif IFD, the GPS IFD, and
/// IFD1 as thumbnail properties (probe exif2.jpg).
void exif_properties(std::span<const u8> tiff, std::vector<Property>& props, float* dpi_x, float* dpi_y,
                     bool* real_dpi, bool with_ifd1) {
    if (tiff.size() < 8) return;
    Tiff t{tiff, tiff[0] == 'I'};
    u32 next = 0;
    const auto ifd0 = read_ifd(t, t.r32(4), &next);
    for (const auto& e : ifd0) {
        if (e.tag == kExifIfd || e.tag == kGpsIfd || e.tag == kInteropIfd) continue;
        props.push_back(e.tag == 0x9286 ? user_comment(e) : to_property(e));
    }
    for (u16 ptr : {kExifIfd, kGpsIfd}) {
        if (const auto* p = find(ifd0, ptr)) {
            for (const auto& e : read_ifd(t, entry_uint(*p), nullptr)) {
                if (e.tag == kInteropIfd) continue;
                props.push_back(e.tag == 0x9286 ? user_comment(e) : to_property(e));
            }
        }
    }
    if (with_ifd1 && next != 0) {
        const auto ifd1 = read_ifd(t, next, nullptr);
        const auto* off = find(ifd1, 0x201);
        const auto* len = find(ifd1, 0x202);
        if (off && len) {
            const u32 o = entry_uint(*off), l = entry_uint(*len);
            if (o + static_cast<u64>(l) <= tiff.size())
                props.push_back(Property{0x501B, PropertyTypeByte,
                                         std::vector<u8>(tiff.begin() + o, tiff.begin() + o + l)});
        }
        for (const auto& e : ifd1) {
            Property p = to_property(e);
            p.id = thumbnail_tag(e.tag);
            props.push_back(std::move(p));
        }
    }
    if (dpi_x && dpi_y) {
        const auto* xr = find(ifd0, 0x11A);
        const auto* yr = find(ifd0, 0x11B);
        const auto* unit = find(ifd0, 0x128);
        const u32 u = unit ? entry_uint(*unit) : 2;
        if (xr && yr && xr->value.size() >= 8 && yr->value.size() >= 8 && (u == 2 || u == 3)) {
            const double xn = le32(xr->value, 0), xd = le32(xr->value, 4), yn = le32(yr->value, 0), yd = le32(yr->value, 4);
            if (xd > 0 && yd > 0 && xn > 0 && yn > 0) {
                const double k = u == 3 ? 2.54 : 1.0;
                *dpi_x = static_cast<float>(xn / xd * k);
                *dpi_y = static_cast<float>(yn / yd * k);
                if (real_dpi) *real_dpi = true;
            }
        }
    }
}

// ---- PNG ------------------------------------------------------------------------------------

bool inflate_all(std::span<const u8> in,std::vector<u8>& out,usize expected){
 return graphics::png_decode::inflate_all(in,out,expected);
}

/// ICC profile description ('desc' textDescriptionType / 'mluc'), or empty.
std::string icc_description(const std::vector<u8>& p) {
    if (p.size() < 132 || be32(p, 0) != p.size()) return {};
    const u32 n = be32(p, 128);
    for (u32 i = 0; i < n && 132 + i * 12 + 12 <= p.size(); ++i) {
        const usize e = 132 + i * 12;
        if (be32(p, e) != 0x64657363) continue;  // 'desc'
        const u32 off = be32(p, e + 4), len = be32(p, e + 8);
        if (off + static_cast<u64>(len) > p.size() || len < 12) return {};
        const u32 type = be32(p, off);
        if (type == 0x64657363) {  // textDescriptionType: count, ASCII
            const u32 c = be32(p, off + 8);
            std::string s;
            for (u32 k = 0; k < c && off + 12 + k < p.size() && p[off + 12 + k]; ++k) s.push_back(static_cast<char>(p[off + 12 + k]));
            return s;
        }
        if (type == 0x6D6C7563 && len >= 28) {  // multiLocalizedUnicodeType: first record
            const u32 sl = be32(p, off + 20), so = be32(p, off + 24);
            std::string s;
            for (u32 k = 0; k + 1 < sl && off + so + k + 1 < p.size(); k += 2) {
                const u16 ch = be16(p, off + so + k);
                s.push_back(ch < 128 ? static_cast<char>(ch) : '?');
            }
            return s;
        }
    }
    return {};
}

/// tEXt keyword -> property id (probe chunks.png/meta.png; unlisted keywords are not reported).
u32 png_text_tag(std::string_view key) {
    if (key == "Title") return 0x0320;
    if (key == "Author") return 0x013B;
    if (key == "Description") return 0x010E;
    if (key == "Copyright") return 0x8298;
    if (key == "Software") return 0x0131;
    if (key == "Source") return 0x0110;
    if (key == "Comment") return 0x9286;
    return 0;
}

Status decode_png(std::span<const u8> b, Decoded& out) {
    if (b.size() < 33 || be32(b, 12) != 0x49484452) return Status::OutOfMemory;
    const u32 w = be32(b, 16), h = be32(b, 20);
    const u8 bd = b[24], ct = b[25], interlace = b[28];
    if (w == 0 || h == 0 || w > 65535 || h > 65535) return Status::OutOfMemory;
    std::vector<u8> idat, plte, trns;
    std::vector<Property> props, icc_props;
    u32 phys_x = 0, phys_y = 0;
    bool phys = false;
    for (usize at = 8; at + 12 <= b.size();) {
        const u32 len = be32(b, at), type = be32(b, at + 4);
        if (at + 12 + static_cast<u64>(len) > b.size()) break;
        std::span<const u8> d = b.subspan(at + 8, len);
        switch (type) {
            case 0x49444154: idat.insert(idat.end(), d.begin(), d.end()); break;   // IDAT
            case 0x504C5445: plte.assign(d.begin(), d.end()); break;               // PLTE
            case 0x74524E53: trns.assign(d.begin(), d.end()); break;               // tRNS
            case 0x67414D41:                                                         // gAMA
                if (len >= 4) props.push_back({0x0301, PropertyTypeRational, rational(100000, be32(d, 0))});
                break;
            case 0x6348524D:                                                         // cHRM
                if (len >= 32) {
                    std::vector<u8> wp, pr;
                    for (int i = 0; i < 2; ++i) { auto r = rational(be32(d, i * 4), 100000); wp.insert(wp.end(), r.begin(), r.end()); }
                    for (int i = 2; i < 8; ++i) { auto r = rational(be32(d, i * 4), 100000); pr.insert(pr.end(), r.begin(), r.end()); }
                    props.push_back({0x013E, PropertyTypeRational, wp});
                    props.push_back({0x013F, PropertyTypeRational, pr});
                }
                break;
            case 0x73524742:                                                         // sRGB
                if (len >= 1) props.push_back({0x0303, PropertyTypeByte, {d[0]}});
                break;
            case 0x74494D45:                                                         // tIME
                if (len >= 7) {
                    char s[32];
                    std::snprintf(s, sizeof s, "%04u:%02u:%02u %02u:%02u:%02u", be16(d, 0), d[2], d[3], d[4], d[5], d[6]);
                    props.push_back({0x0132, PropertyTypeASCII, ascii(s)});
                }
                break;
            case 0x74455874: {                                                       // tEXt
                const auto nul = std::find(d.begin(), d.end(), u8{0});
                if (nul != d.end()) {
                    const std::string key(d.begin(), nul);
                    if (const u32 tag = png_text_tag(key))
                        props.push_back({tag, PropertyTypeASCII, ascii(std::string(nul + 1, d.end()))});
                }
                break;
            }
            case 0x69434350: {                                                       // iCCP
                const auto nul = std::find(d.begin(), d.end(), u8{0});
                std::vector<u8> prof;
                if (nul != d.end() && nul + 2 <= d.end())
                    (void)inflate_all(std::span<const u8>(&*(nul + 2), static_cast<usize>(d.end() - (nul + 2))), prof, 0);
                const std::string desc = icc_description(prof);
                if (desc.empty()) prof.clear();
                icc_props.push_back({0x0302, PropertyTypeASCII, ascii(desc.empty() ? "ICC Profile" : desc)});
                icc_props.push_back({0x8773, PropertyTypeByte, prof});
                break;
            }
            case 0x70485973:                                                         // pHYs
                if (len >= 9 && d[8] == 1) { phys = true; phys_x = be32(d, 0); phys_y = be32(d, 4); }
                break;
            default: break;
        }
        if (type == 0x49454E44) break;
        at += 12 + len;
    }
    const u32 channels = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
    if (channels == 0) return Status::OutOfMemory;
    graphics::png_decode::Pixels native_samples;
    if(!graphics::png_decode::samples({w,h,bd,ct,interlace},idat,native_samples))return Status::OutOfMemory;
    std::vector<u32> argb(static_cast<usize>(w) * h, 0);
    std::vector<i32> index(ct == 3 ? static_cast<usize>(w) * h : 0, 0);
    auto palette_color = [&](u32 i) {
        u32 c = kAlphaMask;
        if (i * 3 + 2 < plte.size()) c |= (u32{plte[i * 3]} << 16) | (u32{plte[i * 3 + 1]} << 8) | plte[i * 3 + 2];
        if (i < trns.size()) c = (c & 0xFFFFFF) | (u32{trns[i]} << 24);
        return c;
    };
    auto sample = [&](const u8* row, usize x, usize c) -> u32 {
        const usize bit = (x * channels + c) * bd;
        if (bd == 16) return static_cast<u32>(row[bit / 8]);                       // high byte
        if (bd == 8) return row[bit / 8];
        const u32 v = (row[bit / 8] >> (8 - bd - (bit % 8))) & ((1u << bd) - 1);
        return v;
    };
    auto put = [&](usize x, usize y, const u8* row, usize rx) {
        const usize o = y * w + x;
        auto scale = [&](u32 v) { return bd >= 8 ? v : v * 255 / ((1u << bd) - 1); };
        switch (ct) {
            case 0: {
                const u32 g = scale(sample(row, rx, 0));
                u32 a = 0xFF;
                if (trns.size() >= 2 && (bd == 16 ? static_cast<u32>(be16(trns, 0)) == ((u32{row[rx * 2]} << 8) | row[rx * 2 + 1])
                                                  : sample(row, rx, 0) == be16(trns, 0))) a = 0;
                argb[o] = (a << 24) | (g << 16) | (g << 8) | g;
                break;
            }
            case 2: {
                u32 a = 0xFF;
                if (trns.size() >= 6) {
                    bool eq = true;
                    for (usize c = 0; c < 3; ++c) {
                        const u32 v = bd == 16 ? static_cast<u32>((row[rx * 6 + c * 2] << 8) | row[rx * 6 + c * 2 + 1]) : sample(row, rx, c);
                        if (v != be16(trns, c * 2)) eq = false;
                    }
                    if (eq) a = 0;
                }
                argb[o] = (a << 24) | (sample(row, rx, 0) << 16) | (sample(row, rx, 1) << 8) | sample(row, rx, 2);
                break;
            }
            case 3: { const u32 i = sample(row, rx, 0); index[o] = static_cast<i32>(i); argb[o] = palette_color(i); break; }
            case 4: { const u32 g = sample(row, rx, 0); argb[o] = (sample(row, rx, 1) << 24) | (g << 16) | (g << 8) | g; break; }
            case 6:
                argb[o] = (sample(row, rx, 3) << 24) | (sample(row, rx, 0) << 16) | (sample(row, rx, 1) << 8) | sample(row, rx, 2);
                break;
        }
    };
    for(usize y=0;y<h;++y)for(usize x=0;x<w;++x)
        put(x,y,native_samples.rows.data()+y*native_samples.stride,x);
    DecodedFrame& f = out.frame;
    // Probe: 8-bit RGB without tRNS -> 24bppRGB; 1/4-bit palette without
    // tRNS -> 1/4bppIndexed; everything else -> 32bppARGB.
    i32 format = PixelFormat32bppARGB;
    if (ct == 2 && bd == 8 && trns.empty()) format = PixelFormat24bppRGB;
    if (ct == 3 && (bd == 1 || bd == 4) && trns.empty()) {
        format = bd == 1 ? PixelFormat1bppIndexed : PixelFormat4bppIndexed;
        for (u32 i = 0; i < (1u << bd); ++i) if (i * 3 + 2 < plte.size()) f.palette.push_back(palette_color(i));
        f.width = static_cast<i32>(w); f.height = static_cast<i32>(h); f.pixel_format = format;
        f.stride = stride_for(f.width, format);
        f.bits.assign(static_cast<usize>(f.stride) * h, 0);
        for (usize y = 0; y < h; ++y)
            for (usize x = 0; x < w; ++x) {
                u8* row = f.bits.data() + y * f.stride;
                const u32 i = static_cast<u32>(index[y * w + x]);
                if (bd == 1) row[x >> 3] |= static_cast<u8>((i & 1) << (7 - (x & 7)));
                else row[x >> 1] |= static_cast<u8>((x & 1) ? (i & 15) : ((i & 15) << 4));
            }
    } else {
        frame_from_argb(f, argb, static_cast<i32>(w), static_cast<i32>(h), format);
    }
    if (phys) { f.dpi_x = phys_x * 0.0254f; f.dpi_y = phys_y * 0.0254f; }
    finish_frame(f, ct == 0 ? kFlagGray : kFlagRGB, phys);
    // Probe order: ICC items first, then the chunks in file order, the pixel-unit trio last.
    f.properties = icc_props;
    f.properties.insert(f.properties.end(), props.begin(), props.end());
    f.properties.push_back({0x5110, PropertyTypeByte, {1}});
    f.properties.push_back({0x5111, PropertyTypeLong, u32v(phys ? phys_x : 0)});
    f.properties.push_back({0x5112, PropertyTypeLong, u32v(phys ? phys_y : 0)});
    return Status::Ok;
}

// ---- GIF ---------------------------------------------------------------------------------------

struct GifFrame {
    u16 x, y, w, h;
    bool interlaced;
    std::vector<u32> palette;   ///< local or global, RGB with alpha FF
    i32 transparent{-1};
    u16 delay{0};
    u8 disposal{0};
    std::vector<u8> indices;
};

struct GifFile {
    u16 width{0}, height{0};
    std::vector<u8> global_rgb;
    u8 background{0};
    bool has_loop{false};
    u16 loop{0};
    std::vector<GifFrame> frames;
};

bool lzw_decode(std::span<const u8> data, u8 min_size, usize count, std::vector<u8>& out) {
    if (min_size < 2 || min_size > 11) return false;
    const u32 clear = 1u << min_size, eoi = clear + 1;
    std::vector<u16> prefix(4096);
    std::vector<u8> suffix(4096), stack;
    u32 size = min_size + 1, next = eoi + 1, prev = 0xFFFF;
    u8 first = 0;
    u32 acc = 0, nbits = 0;
    out.clear();
    for (usize i = 0; i < data.size() && out.size() < count; ++i) {
        acc |= u32{data[i]} << nbits;
        nbits += 8;
        while (nbits >= size && out.size() < count) {
            u32 code = acc & ((1u << size) - 1);
            acc >>= size;
            nbits -= size;
            if (code == clear) { size = min_size + 1; next = eoi + 1; prev = 0xFFFF; continue; }
            if (code == eoi) return true;
            if (prev == 0xFFFF) {
                if (code >= clear) return false;
                out.push_back(static_cast<u8>(code));
                first = static_cast<u8>(code);
                prev = code;
                continue;
            }
            u32 c = code;
            stack.clear();
            if (code >= next) { stack.push_back(first); c = prev; }
            while (c >= clear) { if (c >= 4096) return false; stack.push_back(suffix[c]); c = prefix[c]; }
            stack.push_back(static_cast<u8>(c));
            first = static_cast<u8>(c);
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) out.push_back(*it);
            if (next < 4096) {
                prefix[next] = static_cast<u16>(prev);
                suffix[next] = first;
                ++next;
                if (next == (1u << size) && size < 12) ++size;
            }
            prev = code;
        }
    }
    return true;
}

bool parse_gif(std::span<const u8> b, GifFile& g) {
    if (b.size() < 13) return false;
    g.width = le16(b, 6);
    g.height = le16(b, 8);
    const u8 flags = b[10];
    g.background = b[11];
    usize at = 13;
    if (flags & 0x80) {
        const usize n = 3u << ((flags & 7) + 1);
        if (at + n > b.size()) return false;
        g.global_rgb.assign(b.begin() + at, b.begin() + at + n);
        at += n;
    }
    auto rgb_palette = [](std::span<const u8> rgb) {
        std::vector<u32> p;
        for (usize i = 0; i + 2 < rgb.size(); i += 3) p.push_back(kAlphaMask | (u32{rgb[i]} << 16) | (u32{rgb[i + 1]} << 8) | rgb[i + 2]);
        return p;
    };
    i32 transparent = -1;
    u16 delay = 0;
    u8 disposal = 0;
    while (at < b.size()) {
        const u8 tag = b[at++];
        if (tag == 0x3B) break;
        if (tag == 0x21) {
            if (at >= b.size()) return false;
            const u8 label = b[at++];
            std::vector<u8> body;
            bool first_block = true;
            while (at < b.size()) {
                const u8 n = b[at++];
                if (n == 0) break;
                if (at + n > b.size()) return false;
                if (label == 0xF9 && first_block && n >= 4) {
                    disposal = (b[at] >> 2) & 7;
                    delay = le16(b, at + 1);
                    transparent = (b[at] & 1) ? b[at + 3] : -1;
                }
                if (label == 0xFF) body.insert(body.end(), b.begin() + at, b.begin() + at + n);
                first_block = false;
                at += n;
            }
            if (label == 0xFF && body.size() >= 14 && std::memcmp(body.data(), "NETSCAPE2.0", 11) == 0 && body[11] == 1) {
                g.has_loop = true;
                g.loop = static_cast<u16>(body[12] | (body[13] << 8));
            }
            continue;
        }
        if (tag != 0x2C || at + 9 > b.size()) return !g.frames.empty();
        GifFrame f{le16(b, at), le16(b, at + 2), le16(b, at + 4), le16(b, at + 6), (b[at + 8] & 0x40) != 0, {}, transparent, delay, disposal, {}};
        const u8 lf = b[at + 8];
        at += 9;
        if (lf & 0x80) {
            const usize n = 3u << ((lf & 7) + 1);
            if (at + n > b.size()) return false;
            f.palette = rgb_palette(b.subspan(at, n));
            at += n;
        } else {
            f.palette = rgb_palette(g.global_rgb);
        }
        if (at >= b.size()) return false;
        const u8 min = b[at++];
        std::vector<u8> data;
        while (at < b.size()) {
            const u8 n = b[at++];
            if (n == 0) break;
            if (at + n > b.size()) return false;
            data.insert(data.end(), b.begin() + at, b.begin() + at + n);
            at += n;
        }
        std::vector<u8> idx;
        (void)lzw_decode(data, min, static_cast<usize>(f.w) * f.h, idx);
        idx.resize(static_cast<usize>(f.w) * f.h, 0);
        if (f.interlaced) {
            std::vector<u8> d(idx.size());
            usize src = 0;
            for (int pass = 0; pass < 4; ++pass) {
                static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
                for (usize y = start[pass]; y < f.h; y += step[pass], ++src)
                    std::memcpy(d.data() + y * f.w, idx.data() + src * f.w, f.w);
            }
            idx.swap(d);
        }
        f.indices = std::move(idx);
        g.frames.push_back(std::move(f));
        transparent = -1;
        delay = 0;
        disposal = 0;
    }
    return !g.frames.empty();
}

std::vector<Property> gif_properties(const GifFile& g) {
    std::vector<Property> p;
    std::vector<u8> delays;
    for (const auto& f : g.frames) put32(delays, f.delay);
    p.push_back({0x5100, PropertyTypeLong, delays});
    p.push_back({0x5101, PropertyTypeShort, u16v(g.has_loop ? g.loop : 1)});
    if (!g.global_rgb.empty()) p.push_back({0x5102, PropertyTypeByte, g.global_rgb});
    p.push_back({0x5103, PropertyTypeByte, {g.background}});
    if (g.frames[0].transparent >= 0)
        p.push_back({0x5104, PropertyTypeByte, {static_cast<u8>(g.frames[0].transparent)}});
    return p;
}

Status decode_gif(std::span<const u8> b, u32 index, Decoded& out) {
    GifFile g;
    if (!parse_gif(b, g) || g.width == 0 || g.height == 0) return Status::OutOfMemory;
    DecodedFrame& f = out.frame;
    out.dimension = kFrameDimensionTime;
    out.frame_count = static_cast<u32>(g.frames.size());
    if (index >= g.frames.size()) return Status::Win32Error;
    const i32 W = g.width, H = g.height;
    if (g.frames.size() == 1) {
        // Probe single.gif: 8bppIndexed, the frame's color table, the
        // transparent entry with alpha 0 and PaletteFlagsHasAlpha.
        const GifFrame& fr = g.frames[0];
        f.palette = fr.palette;
        if (f.palette.empty()) f.palette = gray_palette(2);
        if (fr.transparent >= 0 && static_cast<usize>(fr.transparent) < f.palette.size()) {
            f.palette[fr.transparent] &= 0x00FFFFFF;
            f.palette_flags = 1;
        }
        f.width = W; f.height = H; f.pixel_format = PixelFormat8bppIndexed;
        f.stride = stride_for(W, f.pixel_format);
        f.bits.assign(static_cast<usize>(f.stride) * H, g.background);
        for (i32 y = 0; y < fr.h; ++y)
            for (i32 x = 0; x < fr.w; ++x)
                if (fr.x + x < W && fr.y + y < H)
                    f.bits[static_cast<usize>(fr.y + y) * f.stride + fr.x + x] = fr.indices[static_cast<usize>(y) * fr.w + x];
        f.flags = kDecodedBase | kFlagRGB | kFlagRealDpi | (fr.transparent >= 0 ? kFlagHasAlpha : 0);
    } else {
        // Probe anim/partial.gif: 32bppARGB, frames composited with disposal.
        std::vector<u32> canvas(static_cast<usize>(W) * H, 0), saved;
        for (u32 k = 0; k <= index; ++k) {
            const GifFrame& fr = g.frames[k];
            if (fr.disposal == 3) saved = canvas;
            for (i32 y = 0; y < fr.h; ++y)
                for (i32 x = 0; x < fr.w; ++x) {
                    const i32 cx = fr.x + x, cy = fr.y + y;
                    if (cx >= W || cy >= H) continue;
                    const u8 i = fr.indices[static_cast<usize>(y) * fr.w + x];
                    if (i == fr.transparent) continue;
                    canvas[static_cast<usize>(cy) * W + cx] = i < fr.palette.size() ? fr.palette[i] : kAlphaMask;
                }
            if (k == index) break;
            if (fr.disposal == 2) {
                for (i32 y = 0; y < fr.h; ++y)
                    for (i32 x = 0; x < fr.w; ++x)
                        if (fr.x + x < W && fr.y + y < H) canvas[static_cast<usize>(fr.y + y) * W + fr.x + x] = 0;
            } else if (fr.disposal == 3 && !saved.empty()) {
                canvas = saved;
            }
        }
        frame_from_argb(f, canvas, W, H, PixelFormat32bppARGB);
        f.flags = kDecodedBase | kFlagRGB | kFlagRealDpi;
    }
    f.properties = gif_properties(g);
    return Status::Ok;
}

// ---- TIFF -------------------------------------------------------------------------------------------

std::vector<usize> tiff_ifds(std::span<const u8> b) {
    std::vector<usize> out;
    if (b.size() < 8) return out;
    Tiff t{b, b[0] == 'I'};
    u32 at = t.r32(4);
    while (at != 0 && at < b.size() && out.size() < 4096) {
        if (std::find(out.begin(), out.end(), at) != out.end()) break;
        out.push_back(at);
        u32 next = 0;
        (void)read_ifd(t, at, &next);
        at = next;
    }
    return out;
}

Status decode_tiff(std::span<const u8> b, u32 index, Decoded& out) {
    const auto ifds = tiff_ifds(b);
    if (ifds.empty()) return Status::OutOfMemory;
    out.dimension = kFrameDimensionPage;
    out.frame_count = static_cast<u32>(ifds.size());
    if (index >= ifds.size()) return Status::Win32Error;
    Tiff t{b, b[0] == 'I'};
    const auto ifd = read_ifd(t, ifds[index], nullptr);
    auto tag = [&](u16 id, u32 dflt) { const auto* e = find(ifd, id); return e ? entry_uint(*e) : dflt; };
    const u32 photometric = tag(0x106, 1), spp = tag(0x115, 1), bps = tag(0x102, 1);
    const bool extra = find(ifd, 0x152) != nullptr;
    DecodedFrame& f = out.frame;
    i32 w = 0, h = 0;
    std::vector<u32> argb;
    if (!imageio_decode(b, index, w, h, argb)) return Status::OutOfMemory;
    u32 colorspace = kFlagRGB;
    i32 format = PixelFormat24bppRGB;
    if (photometric == 3 && bps <= 8) {
        const auto* cmap = find(ifd, 0x140);
        const u32 n = 1u << bps;
        for (u32 i = 0; i < n; ++i) {
            const u32 r = cmap ? entry_uint(*cmap, i) >> 8 : 0, g = cmap ? entry_uint(*cmap, n + i) >> 8 : 0,
                      bl = cmap ? entry_uint(*cmap, 2 * n + i) >> 8 : 0;
            f.palette.push_back(kAlphaMask | (r << 16) | (g << 8) | bl);
        }
        format = bps == 1 ? PixelFormat1bppIndexed : bps == 4 ? PixelFormat4bppIndexed : PixelFormat8bppIndexed;
    } else if ((photometric == 0 || photometric == 1) && spp == 1 && bps <= 8) {
        format = bps == 1 ? PixelFormat1bppIndexed : bps == 4 ? PixelFormat4bppIndexed : PixelFormat8bppIndexed;
        f.palette = gray_palette(1u << (bps == 1 ? 1 : bps == 4 ? 4 : 8));
        if (photometric == 0) std::reverse(f.palette.begin(), f.palette.end());
        colorspace = kFlagGray;
        if (bps > 1) f.palette_flags = 2;   // PaletteFlagsGrayScale
    } else if (photometric == 5) {
        colorspace = kFlagCMYK;
        format = PixelFormat24bppRGB;
    } else {
        if (photometric == 6) colorspace = kFlagYCbCr;
        format = (extra || spp == 4 || spp == 2) ? PixelFormat32bppARGB : PixelFormat24bppRGB;
    }
    frame_from_argb(f, argb, w, h, format);
    bool real = false;
    float dx = 96, dy = 96;
    // Every tag of the page's IFD, in IFD order, is a property (probe two.tif);
    // EXIF/GPS sub-IFDs expand in place of their pointers.
    for (const auto& e : ifd) {
        if (e.tag == kExifIfd || e.tag == kGpsIfd) {
            for (const auto& s : read_ifd(t, entry_uint(e), nullptr)) f.properties.push_back(s.tag == 0x9286 ? user_comment(s) : to_property(s));
            continue;
        }
        f.properties.push_back(to_property(e));
    }
    const auto* xr = find(ifd, 0x11A);
    const auto* yr = find(ifd, 0x11B);
    const u32 unit = tag(0x128, 2);
    if (xr && yr && xr->value.size() >= 8 && le32(xr->value, 4) && le32(yr->value, 4) && unit != 1) {
        const float k = unit == 3 ? 2.54f : 1.0f;
        dx = static_cast<float>(le32(xr->value, 0)) / static_cast<float>(le32(xr->value, 4)) * k;
        dy = static_cast<float>(le32(yr->value, 0)) / static_cast<float>(le32(yr->value, 4)) * k;
        real = dx > 0 && dy > 0;
    }
    if (real) { f.dpi_x = dx; f.dpi_y = dy; }
    finish_frame(f, colorspace, real);
    return Status::Ok;
}

// ---- JPEG -------------------------------------------------------------------------------------------

constexpr u8 kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                            41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                            30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct JpegInfo {
    u32 components{3};
    std::vector<u8> exif;      ///< the TIFF body of the Exif APP1
    std::vector<u8> tables[4]; ///< DQT tables, natural order, as SHORT values
    bool have[4]{};
    u8 density_unit{0};
    u16 density_x{0}, density_y{0};
    bool adobe_ycck{false};
};

JpegInfo parse_jpeg(std::span<const u8> b) {
    JpegInfo j;
    for (usize at = 2; at + 4 <= b.size();) {
        if (b[at] != 0xFF) { ++at; continue; }
        const u8 marker = b[at + 1];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01 || marker == 0xFF) { at += marker == 0xFF ? 1 : 2; continue; }
        const u16 len = be16(b, at + 2);
        if (len < 2 || at + 2 + len > b.size()) break;
        std::span<const u8> d = b.subspan(at + 4, len - 2);
        if (marker == 0xE1 && d.size() > 6 && std::memcmp(d.data(), "Exif\0\0", 6) == 0 && j.exif.empty()) {
            j.exif.assign(d.begin() + 6, d.end());
        } else if (marker == 0xE0 && d.size() >= 12 && std::memcmp(d.data(), "JFIF\0", 5) == 0) {
            j.density_unit = d[7];
            j.density_x = be16(d, 8);
            j.density_y = be16(d, 10);
        } else if (marker == 0xDB) {
            for (usize p = 0; p < d.size();) {
                const u8 pq = d[p] >> 4, tq = d[p] & 3;
                const usize n = pq ? 128 : 64;
                if (p + 1 + n > d.size()) break;
                std::vector<u8> v(128);
                for (int k = 0; k < 64; ++k) {
                    const u16 q = pq ? be16(d, p + 1 + k * 2) : d[p + 1 + k];
                    v[kZigzag[k] * 2] = static_cast<u8>(q);
                    v[kZigzag[k] * 2 + 1] = static_cast<u8>(q >> 8);
                }
                j.tables[tq] = v;
                j.have[tq] = true;
                p += 1 + n;
            }
        } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (d.size() >= 6) j.components = d[5];
        } else if (marker == 0xEE && d.size() >= 12 && std::memcmp(d.data(), "Adobe", 5) == 0) {
            j.adobe_ycck = d[11] == 2;
        } else if (marker == 0xDA) {
            break;
        }
        at += 2 + len;
    }
    return j;
}

Status decode_jpeg(std::span<const u8> b, Decoded& out) {
    const JpegInfo j = parse_jpeg(b);
    DecodedFrame& f = out.frame;
    i32 w = 0, h = 0;
    std::vector<u32> argb;
    if (!imageio_decode(b, 0, w, h, argb)) return Status::OutOfMemory;
    bool real = false;
    if (j.density_unit == 1 || j.density_unit == 2) {
        const float k = j.density_unit == 2 ? 2.54f : 1.0f;
        if (j.density_x && j.density_y) { f.dpi_x = j.density_x * k; f.dpi_y = j.density_y * k; real = true; }
    }
    exif_properties(j.exif, f.properties, &f.dpi_x, &f.dpi_y, &real, true);
    u32 colorspace = kFlagRGB;
    if (j.components == 1) {
        // Probe gray.jpg: 8bppIndexed over a 256-gray palette, PaletteFlagsGrayScale.
        f.palette = gray_palette(256);
        f.palette_flags = 2;
        frame_from_argb(f, argb, w, h, PixelFormat8bppIndexed);
        colorspace = kFlagGray;
    } else {
        frame_from_argb(f, argb, w, h, PixelFormat24bppRGB);
        if (j.components == 4) colorspace = j.adobe_ycck ? 0x0100 : kFlagCMYK;
    }
    // Probe: chrominance table (0x5091) then luminance (0x5090).
    if (j.have[1]) f.properties.push_back({0x5091, PropertyTypeShort, j.tables[1]});
    if (j.have[0]) f.properties.push_back({0x5090, PropertyTypeShort, j.tables[0]});
    finish_frame(f, colorspace, real);
    return Status::Ok;
}

Status decode_icon(std::span<const u8> b, Decoded& out) {
    // ICONDIR / ICONDIRENTRY (Learn "Icons"); the first image is decoded.
    // Each entry holds a PNG or a DIB whose height covers XOR + AND masks.
    if (b.size() < 22 || le16(b, 4) == 0) return Status::OutOfMemory;
    const u32 size = le32(b, 14), off = le32(b, 18);
    if (off + static_cast<u64>(size) > b.size() || size < 40) return Status::OutOfMemory;
    std::span<const u8> img = b.subspan(off, size);
    std::vector<u32> argb;
    i32 w = 0, h = 0;
    if (img.size() >= 8 && std::memcmp(img.data(), "\x89PNG", 4) == 0) {
        Decoded d;
        if (decode_png(img, d) != Status::Ok) return Status::OutOfMemory;
        GpImage tmp;
        w = d.frame.width; h = d.frame.height;
        // Read back the PNG frame as ARGB through a scratch image.
        apply_frame(tmp, std::move(d.frame));
        std::vector<u8> rows = tmp.bits;
        argb.resize(static_cast<usize>(w) * h);
        for (i32 y = 0; y < h; ++y)
            for (i32 x = 0; x < w; ++x) {
                const u8* r = rows.data() + static_cast<usize>(y) * tmp.stride;
                u32 c = 0;
                if (tmp.pixel_format == PixelFormat24bppRGB) c = kAlphaMask | (u32{r[x * 3 + 2]} << 16) | (u32{r[x * 3 + 1]} << 8) | r[x * 3];
                else if (tmp.pixel_format == PixelFormat32bppARGB) std::memcpy(&c, r + x * 4, 4);
                else if (tmp.pixel_format == PixelFormat1bppIndexed) c = tmp.palette[(r[x >> 3] >> (7 - (x & 7))) & 1];
                else c = tmp.palette[(x & 1) ? (r[x >> 1] & 15) : (r[x >> 1] >> 4)];
                argb[static_cast<usize>(y) * w + x] = c;
            }
    } else {
        const u32 hsize = le32(img, 0);
        w = static_cast<i32>(le32(img, 4));
        h = static_cast<i32>(le32(img, 8)) / 2;
        const u16 bpp = le16(img, 14);
        if (hsize < 40 || w <= 0 || h <= 0) return Status::OutOfMemory;
        std::vector<u8> file = {'B', 'M'};
        const u32 colors = bpp <= 8 ? (le32(img, 32) ? le32(img, 32) : (1u << bpp)) : 0;
        const u32 xor_stride = ((static_cast<u32>(w) * bpp + 31) / 32) * 4;
        const u32 bits_at = hsize + colors * 4;
        put32(file, 14 + size); put32(file, 0); put32(file, 14 + bits_at);
        file.insert(file.end(), img.begin(), img.end());
        // Patch the height to the XOR image's.
        set32(file, 14 + 8, static_cast<u32>(h));
        Decoded d;
        if (decode_bmp(file, d) != Status::Ok) return Status::OutOfMemory;
        GpImage tmp;
        apply_frame(tmp, std::move(d.frame));
        argb.resize(static_cast<usize>(w) * h);
        for (i32 y = 0; y < h; ++y)
            for (i32 x = 0; x < w; ++x) {
                const u8* r = tmp.bits.data() + static_cast<usize>(y) * tmp.stride;
                u32 c = 0;
                switch (tmp.pixel_format) {
                    case PixelFormat24bppRGB: c = kAlphaMask | (u32{r[x * 3 + 2]} << 16) | (u32{r[x * 3 + 1]} << 8) | r[x * 3]; break;
                    case PixelFormat32bppRGB: std::memcpy(&c, r + x * 4, 4); c |= kAlphaMask; break;
                    case PixelFormat32bppARGB: std::memcpy(&c, r + x * 4, 4); break;
                    case PixelFormat8bppIndexed: c = tmp.palette[r[x]]; break;
                    case PixelFormat4bppIndexed: c = tmp.palette[(x & 1) ? (r[x >> 1] & 15) : (r[x >> 1] >> 4)]; break;
                    default: c = tmp.palette[(r[x >> 3] >> (7 - (x & 7))) & 1]; break;
                }
                argb[static_cast<usize>(y) * w + x] = c;
            }
        // 32bpp icons carry alpha in the XOR image; the rest use the AND mask
        // (bottom-up, 1 = transparent).
        const bool alpha32 = bpp == 32;
        const u32 and_stride = ((static_cast<u32>(w) + 31) / 32) * 4;
        const usize and_at = bits_at + static_cast<usize>(xor_stride) * h;
        if (!alpha32 && and_at + static_cast<usize>(and_stride) * h <= img.size())
            for (i32 y = 0; y < h; ++y)
                for (i32 x = 0; x < w; ++x)
                    if ((img[and_at + static_cast<usize>(h - 1 - y) * and_stride + (x >> 3)] >> (7 - (x & 7))) & 1)
                        argb[static_cast<usize>(y) * w + x] = 0;
        if (alpha32) {
            // A 32bpp DIB in an icon is BI_RGB with alpha in the 4th byte.
            for (i32 y = 0; y < h; ++y)
                for (i32 x = 0; x < w; ++x) {
                    const usize at = bits_at + static_cast<usize>(h - 1 - y) * xor_stride + x * 4u + 3;
                    if (at < img.size()) argb[static_cast<usize>(y) * w + x] = (argb[static_cast<usize>(y) * w + x] & 0xFFFFFF) | (u32{img[at]} << 24);
                }
        }
    }
    frame_from_argb(out.frame, argb, w, h, PixelFormat32bppARGB);
    finish_frame(out.frame, kFlagRGB, false);
    return Status::Ok;
}

// ---- encoders: shared -----------------------------------------------------------------------------------

i32 indexed_bits(i32 f) {
    return f == PixelFormat1bppIndexed ? 1 : f == PixelFormat4bppIndexed ? 4 : f == PixelFormat8bppIndexed ? 8 : 0;
}

u32 nearest(const std::vector<u32>& pal, u32 c, bool use_alpha) {
    u32 best = 0;
    i64 bd = -1;
    for (u32 i = 0; i < pal.size(); ++i) {
        if (pal[i] == c) return i;
        i64 d = 0;
        for (int s = 0; s < (use_alpha ? 32 : 24); s += 8) {
            const i64 k = static_cast<i64>((pal[i] >> s) & 0xFF) - static_cast<i64>((c >> s) & 0xFF);
            d += k * k;
        }
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return best;
}

}  // namespace

const std::vector<u32>& gif_halftone_palette() {
    static const std::vector<u32> pal = [] {
        std::vector<u32> p;
        for (u32 r = 0; r < 6; ++r)
            for (u32 g = 0; g < 7; ++g)
                for (u32 b = 0; b < 6; ++b)
                    p.push_back(kAlphaMask | ((r * 51) << 16) | ((static_cast<u32>(std::lround(g * 255.0 / 6.0))) << 8) | (b * 51));
        p.push_back(0x00000000);
        while (p.size() < 256) p.push_back(kAlphaMask);
        return p;
    }();
    return pal;
}

std::optional<SourceFormat> sniff(std::span<const u8> b) {
    auto at = [&](usize i, std::string_view s) { return b.size() >= i + s.size() && std::memcmp(b.data() + i, s.data(), s.size()) == 0; };
    if (at(0, "BM")) return SourceFormat::Bmp;
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xD8) return SourceFormat::Jpeg;
    if (at(0, "GIF89a") || at(0, "GIF87a")) return SourceFormat::Gif;
    if (at(40, " EMF")) return SourceFormat::Emf;
    if (b.size() >= 4 && le32(b, 0) == 0x9AC6CDD7) return SourceFormat::Wmf;
    if (at(0, "II") || at(0, "MM")) return SourceFormat::Tiff;
    if (at(0, "\x89PNG\r\n\x1a\n")) return SourceFormat::Png;
    if (b.size() >= 4 && le32(b, 0) == 0x00010000) return SourceFormat::Icon;
    return std::nullopt;
}

Status decode(SourceFormat format, std::span<const u8> src, u32 index, Decoded& out) {
    out = Decoded{};
    if (index != 0 && format != SourceFormat::Gif && format != SourceFormat::Tiff) return Status::Win32Error;
    switch (format) {
        case SourceFormat::Bmp: return decode_bmp(src, out);
        case SourceFormat::Png: return decode_png(src, out);
        case SourceFormat::Gif: return decode_gif(src, index, out);
        case SourceFormat::Tiff: return decode_tiff(src, index, out);
        case SourceFormat::Jpeg: return decode_jpeg(src, out);
        case SourceFormat::Icon: return decode_icon(src, out);
        default: return Status::OutOfMemory;
    }
}

bool decode_thumbnail(std::span<const u8> jpeg, DecodedFrame& out) {
    Decoded d;
    if (decode_jpeg(jpeg, d) != Status::Ok) return false;
    out = std::move(d.frame);
    out.properties.clear();
    return true;
}

// ---- BMP encoder (probe save[0] / r2.savefmt enc0) -----------------------------------------------------------

Status encode_bmp(const EncodeFrame& f, std::vector<u8>& out) {
    const i32 sf = f.source_format;
    if (sf == PixelFormat16bppGrayScale) return Status::GenericError;
    i32 bpp = 32, comp = 0;
    std::vector<u32> pal;
    if (const i32 ib = indexed_bits(sf)) { bpp = ib; pal = f.palette; }
    else if (sf == PixelFormat16bppRGB565) { bpp = 16; comp = 3; }
    else if (sf == PixelFormat16bppRGB555) bpp = 16;
    else if (sf == PixelFormat24bppRGB || sf == PixelFormat16bppARGB1555 || sf == PixelFormat48bppRGB) bpp = 24;
    else if (sf == PixelFormat64bppARGB && !f.raw.empty()) bpp = 64;
    const u32 stride = static_cast<u32>(((f.width * bpp + 31) / 32) * 4);
    const u32 pal_bytes = static_cast<u32>(pal.size() * 4) + (comp == 3 ? 12 : 0);
    const u32 off = 54 + pal_bytes, total = off + stride * static_cast<u32>(f.height);
    out.clear();
    out.push_back('B'); out.push_back('M');
    put32(out, total); put32(out, 0); put32(out, off);
    put32(out, 40); put32(out, static_cast<u32>(f.width)); put32(out, static_cast<u32>(f.height));
    put16(out, 1); put16(out, static_cast<u16>(bpp)); put32(out, static_cast<u32>(comp)); put32(out, 0);
    put32(out, static_cast<u32>(std::lround(f.dpi_x / 0.0254))); put32(out, static_cast<u32>(std::lround(f.dpi_y / 0.0254)));
    put32(out, bpp <= 8 && pal.size() < (1u << bpp) ? static_cast<u32>(pal.size()) : 0); put32(out, 0);
    if (comp == 3) { put32(out, 0xF800); put32(out, 0x07E0); put32(out, 0x001F); }
    for (u32 c : pal) put32(out, c & 0x00FFFFFF);
    out.resize(total, 0);
    for (i32 y = 0; y < f.height; ++y) {
        u8* row = out.data() + off + stride * static_cast<u32>(f.height - 1 - y);
        if (bpp == 64) { std::memcpy(row, f.raw.data() + static_cast<usize>(y) * f.width * 8, static_cast<usize>(f.width) * 8); continue; }
        for (i32 x = 0; x < f.width; ++x) {
            const u32 c = f.argb[static_cast<usize>(y) * f.width + x];
            const u32 r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            switch (bpp) {
                case 1: row[x >> 3] |= static_cast<u8>((nearest(pal, c, false) & 1) << (7 - (x & 7))); break;
                case 4: row[x >> 1] |= static_cast<u8>((x & 1) ? (nearest(pal, c, false) & 15) : ((nearest(pal, c, false) & 15) << 4)); break;
                case 8: row[x] = static_cast<u8>(nearest(pal, c, false)); break;
                case 16: {
                    const u16 v = comp == 3 ? static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
                                            : static_cast<u16>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                    row[x * 2] = static_cast<u8>(v); row[x * 2 + 1] = static_cast<u8>(v >> 8);
                    break;
                }
                case 24: row[x * 3] = static_cast<u8>(b); row[x * 3 + 1] = static_cast<u8>(g); row[x * 3 + 2] = static_cast<u8>(r); break;
                default: std::memcpy(row + x * 4, &c, 4); break;   // B,G,R,A with the alpha byte kept
            }
        }
    }
    return Status::Ok;
}

// ---- PNG encoder (probe save[4] / r2.savefmt enc4) ------------------------------------------------------------

namespace {
void png_chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& d) {
    putbe32(out, static_cast<u32>(d.size()));
    const usize start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), d.begin(), d.end());
    putbe32(out, static_cast<u32>(crc32(0, out.data() + start, static_cast<uInt>(out.size() - start))));
}
std::vector<u8> be32v(u32 x) { std::vector<u8> v; putbe32(v, x); return v; }
const Property* prop(const std::vector<Property>& p, u32 id) {
    for (const auto& x : p) if (x.id == id) return &x;
    return nullptr;
}
std::string prop_text(const Property& p) {
    std::string s(p.value.begin(), p.value.end());
    if (const auto z = s.find('\0'); z != std::string::npos) s.resize(z);
    return s;
}
}  // namespace

Status encode_png(const EncodeFrame& f, std::vector<u8>& out) {
    const i32 sf = f.source_format;
    if (sf == PixelFormat16bppGrayScale) return Status::GenericError;
    u8 ct = 6, bd = 8;
    const i32 ib = indexed_bits(sf);
    if (ib) { ct = 3; bd = static_cast<u8>(ib); }
    else if (sf == PixelFormat24bppRGB) ct = 2;
    else if (sf == PixelFormat48bppRGB) { ct = 2; bd = 16; }
    else if (sf == PixelFormat64bppARGB || sf == PixelFormat64bppPARGB) bd = 16;
    out.assign({0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'});
    std::vector<u8> ih = be32v(static_cast<u32>(f.width));
    auto hv = be32v(static_cast<u32>(f.height));
    ih.insert(ih.end(), hv.begin(), hv.end());
    ih.insert(ih.end(), {bd, ct, 0, 0, 0});
    png_chunk(out, "IHDR", ih);
    png_chunk(out, "sRGB", {0});
    png_chunk(out, "gAMA", be32v(45455));
    if (ct == 3) {
        std::vector<u8> pl, tr;
        bool any_alpha = false;
        for (u32 c : f.palette) {
            pl.push_back(static_cast<u8>(c >> 16)); pl.push_back(static_cast<u8>(c >> 8)); pl.push_back(static_cast<u8>(c));
            tr.push_back(static_cast<u8>(c >> 24));
            if ((c >> 24) != 0xFF) any_alpha = true;
        }
        png_chunk(out, "PLTE", pl);
        if (any_alpha) png_chunk(out, "tRNS", tr);
    }
    for (auto [id, key] : {std::pair<u32, const char*>{0x0320, "Title"}, {0x013B, "Author"}, {0x010E, "Description"},
                           {0x8298, "Copyright"}, {0x0131, "Software"}, {0x0110, "Source"}, {0x9286, "Comment"}}) {
        const Property* p = prop(f.properties, id);
        if (!p || p->type != PropertyTypeASCII) continue;
        std::vector<u8> d(key, key + std::strlen(key));
        d.push_back(0);
        const std::string t = prop_text(*p);
        d.insert(d.end(), t.begin(), t.end());
        png_chunk(out, "tEXt", d);
    }
    if (const Property* p = prop(f.properties, 0x0132); p && p->type == PropertyTypeASCII) {
        unsigned Y, M, D, h, m, s;
        if (std::sscanf(prop_text(*p).c_str(), "%u:%u:%u %u:%u:%u", &Y, &M, &D, &h, &m, &s) == 6)
            png_chunk(out, "tIME", {static_cast<u8>(Y >> 8), static_cast<u8>(Y), static_cast<u8>(M), static_cast<u8>(D),
                                    static_cast<u8>(h), static_cast<u8>(m), static_cast<u8>(s)});
    }
    std::vector<u8> phys = be32v(static_cast<u32>(std::lround(f.dpi_x / 0.0254)));
    auto py = be32v(static_cast<u32>(std::lround(f.dpi_y / 0.0254)));
    phys.insert(phys.end(), py.begin(), py.end());
    phys.push_back(1);
    png_chunk(out, "pHYs", phys);
    const usize channels = ct == 2 ? 3 : ct == 6 ? 4 : 1;
    const usize rb = (static_cast<usize>(f.width) * channels * bd + 7) / 8;
    std::vector<u8> raw((rb + 1) * static_cast<usize>(f.height), 0);
    auto lin16 = [](u32 v8) { return static_cast<u16>(v8 * 257); };
    for (i32 y = 0; y < f.height; ++y) {
        u8* row = raw.data() + static_cast<usize>(y) * (rb + 1) + 1;
        for (i32 x = 0; x < f.width; ++x) {
            const u32 c = f.argb[static_cast<usize>(y) * f.width + x];
            const u32 ch[4] = {(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, c >> 24};
            if (ct == 3) {
                const u32 i = nearest(f.palette, c, true);
                if (bd == 8) row[x] = static_cast<u8>(i);
                else if (bd == 4) row[x >> 1] |= static_cast<u8>((x & 1) ? (i & 15) : ((i & 15) << 4));
                else row[x >> 3] |= static_cast<u8>((i & 1) << (7 - (x & 7)));
            } else {
                for (usize k = 0; k < channels; ++k) {
                    if (bd == 16) { const u16 v = lin16(ch[k]); row[(x * channels + k) * 2] = static_cast<u8>(v >> 8); row[(x * channels + k) * 2 + 1] = static_cast<u8>(v); }
                    else row[x * channels + k] = static_cast<u8>(ch[k]);
                }
            }
        }
    }
    uLongf zn = compressBound(static_cast<uLong>(raw.size()));
    std::vector<u8> z(zn);
    if (compress2(z.data(), &zn, raw.data(), static_cast<uLong>(raw.size()), 9) != Z_OK) return Status::GenericError;
    z.resize(zn);
    png_chunk(out, "IDAT", z);
    png_chunk(out, "IEND", {});
    return Status::Ok;
}

// ---- GIF encoder (probe save[2], r2.gifpalette / r2.gifindices) ----------------------------------------------------------

namespace {
void lzw_encode(const std::vector<u8>& idx, u8 min_size, std::vector<u8>& out) {
    const u32 clear = 1u << min_size, eoi = clear + 1;
    std::map<std::pair<u32, u8>, u32> dict;
    u32 size = min_size + 1, next = eoi + 1;
    u32 acc = 0, nbits = 0;
    std::vector<u8> bytes;
    auto emit = [&](u32 code) {
        acc |= code << nbits;
        nbits += size;
        while (nbits >= 8) { bytes.push_back(static_cast<u8>(acc)); acc >>= 8; nbits -= 8; }
    };
    emit(clear);
    if (!idx.empty()) {
        u32 cur = idx[0];
        for (usize i = 1; i < idx.size(); ++i) {
            const auto key = std::make_pair(cur, idx[i]);
            const auto it = dict.find(key);
            if (it != dict.end()) { cur = it->second; continue; }
            emit(cur);
            if (next < 4096) {
                dict[key] = next++;
                if (next > (1u << size) && size < 12) ++size;
            } else {
                emit(clear);
                dict.clear();
                size = min_size + 1;
                next = eoi + 1;
            }
            cur = idx[i];
        }
        emit(cur);
    }
    emit(eoi);
    if (nbits) bytes.push_back(static_cast<u8>(acc));
    out.push_back(min_size);
    for (usize i = 0; i < bytes.size(); i += 255) {
        const usize n = std::min<usize>(255, bytes.size() - i);
        out.push_back(static_cast<u8>(n));
        out.insert(out.end(), bytes.begin() + i, bytes.begin() + i + n);
    }
    out.push_back(0);
}
}  // namespace

Status encode_gif(const std::vector<EncodeFrame>& frames, std::vector<u8>& out) {
    if (frames.empty()) return Status::InvalidParameter;
    const EncodeFrame& f0 = frames[0];
    // An 8bppIndexed source keeps its palette; everything else (1/4bpp
    // indexed too) maps onto the halftone palette (probe r2.savefmt enc2).
    auto palette_of = [](const EncodeFrame& f) {
        return f.source_format == PixelFormat8bppIndexed && !f.palette.empty() ? f.palette : gif_halftone_palette();
    };
    std::vector<u32> gpal = palette_of(f0);
    gpal.resize(256, kAlphaMask);
    out.assign({'G', 'I', 'F', '8', '9', 'a'});
    put16(out, static_cast<u16>(f0.width));
    put16(out, static_cast<u16>(f0.height));
    out.push_back(0xF7);   // global table, 8-bit color resolution, 256 entries
    out.push_back(0);
    out.push_back(0);
    for (u32 c : gpal) { out.push_back(static_cast<u8>(c >> 16)); out.push_back(static_cast<u8>(c >> 8)); out.push_back(static_cast<u8>(c)); }
    if (frames.size() > 1) {
        const u8 ns[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0, 0, 0};
        out.insert(out.end(), std::begin(ns), std::end(ns));
    }
    for (const EncodeFrame& f : frames) {
        std::vector<u32> pal = palette_of(f);
        i32 transparent = -1;
        for (u32 i = 0; i < pal.size(); ++i) if ((pal[i] >> 24) == 0) { transparent = static_cast<i32>(i); break; }
        const bool local = pal != gpal;
        std::vector<u8> idx(static_cast<usize>(f.width) * f.height);
        for (usize i = 0; i < idx.size(); ++i) {
            const u32 c = f.argb[i];
            if ((c >> 24) < 0x80 && transparent >= 0) { idx[i] = static_cast<u8>(transparent); continue; }
            u32 best = 0;
            i64 bd = -1;
            for (u32 k = 0; k < pal.size(); ++k) {
                if (static_cast<i32>(k) == transparent) continue;
                i64 d = 0;
                for (int s = 0; s < 24; s += 8) { const i64 q = static_cast<i64>((pal[k] >> s) & 0xFF) - static_cast<i64>((c >> s) & 0xFF); d += q * q; }
                if (bd < 0 || d < bd) { bd = d; best = k; }
                if (d == 0) break;
            }
            idx[i] = static_cast<u8>(best);
        }
        out.insert(out.end(), {0x21, 0xF9, 0x04, static_cast<u8>(transparent >= 0 ? 1 : 0), 0, 0,
                               static_cast<u8>(transparent >= 0 ? transparent : 0), 0});
        out.push_back(0x2C);
        put16(out, 0); put16(out, 0);
        put16(out, static_cast<u16>(f.width)); put16(out, static_cast<u16>(f.height));
        if (local) {
            pal.resize(256, kAlphaMask);
            out.push_back(0x87);
            for (u32 c : pal) { out.push_back(static_cast<u8>(c >> 16)); out.push_back(static_cast<u8>(c >> 8)); out.push_back(static_cast<u8>(c)); }
        } else {
            out.push_back(0);
        }
        lzw_encode(idx, 8, out);
    }
    out.push_back(0x3B);
    return Status::Ok;
}

// ---- TIFF encoder (probe save[3], param[3], multi[3]) ---------------------------------------------------------

namespace {

/// TIFF LZW (TIFF 6.0 section 13: MSB-first codes, early code-width change).
std::vector<u8> tiff_lzw(const std::vector<u8>& in) {
    std::vector<u8> out;
    std::map<std::pair<u32, u8>, u32> dict;
    u32 size = 9, next = 258, acc = 0, nbits = 0;
    auto emit = [&](u32 code) {
        acc = (acc << size) | code;
        nbits += size;
        while (nbits >= 8) { out.push_back(static_cast<u8>(acc >> (nbits - 8))); nbits -= 8; }
        acc &= (1u << nbits) - 1;
    };
    emit(256);
    if (!in.empty()) {
        u32 cur = in[0];
        for (usize i = 1; i < in.size(); ++i) {
            const auto key = std::make_pair(cur, in[i]);
            const auto it = dict.find(key);
            if (it != dict.end()) { cur = it->second; continue; }
            emit(cur);
            dict[key] = next++;
            if (next + 1 > (1u << size) && size < 12) ++size;
            if (next >= 4093) { emit(256); dict.clear(); next = 258; size = 9; }
            cur = in[i];
        }
        emit(cur);
        ++next;
        if (next + 1 > (1u << size) && size < 12) ++size;
    }
    emit(257);
    if (nbits) out.push_back(static_cast<u8>(acc << (8 - nbits)));
    return out;
}

struct TagOut { u16 tag; u16 type; u32 count; std::vector<u8> value; };

bool is_structural(u32 id) {
    switch (id) {
        case 0xFE: case 0x100: case 0x101: case 0x102: case 0x103: case 0x106: case 0x111: case 0x115: case 0x116:
        case 0x117: case 0x11A: case 0x11B: case 0x11C: case 0x128: case 0x13D: case 0x140: case 0x152: case 0x153:
        case kExifIfd: case kGpsIfd: case kInteropIfd: case 0x201: case 0x202:
            return true;
        default:
            return false;
    }
}

}  // namespace

Status encode_tiff(const std::vector<EncodeFrame>& frames, const EncodeOptions& o, std::vector<u8>& out) {
    if (frames.empty()) return Status::InvalidParameter;
    if (o.tiff_compression == 3 || o.tiff_compression == 4 || o.tiff_compression == 5) {
        log_unimplemented("GdipSaveImageToStream", "TIFF CCITT3 / CCITT4 / RLE compression");
        return Status::NotImplemented;
    }
    out.assign({'I', 'I', 42, 0, 0, 0, 0, 0});
    usize link = 4;
    for (const EncodeFrame& f : frames) {
        // Output layout: the color depth parameter, else the source's (probe r2.savefmt enc3).
        i32 depth = o.color_depth;
        if (depth == 0) {
            const i32 ib = indexed_bits(f.source_format);
            depth = ib ? ib
                  : (f.source_format == PixelFormat24bppRGB || f.source_format == PixelFormat16bppRGB555 ||
                     f.source_format == PixelFormat16bppRGB565 || f.source_format == PixelFormat48bppRGB) ? 24 : 32;
        }
        std::vector<u32> pal;
        if (depth <= 8) {
            if (indexed_bits(f.source_format) == depth && !f.palette.empty()) pal = f.palette;
            else if (depth == 8) pal = gif_halftone_palette();
            else if (depth == 1) pal = {0xFF000000u, 0xFFFFFFFFu};
            else for (u32 i = 0; i < 16; ++i) {
                // 16 colors: the 4-bit VGA layout (black, the 7 dark primaries, gray, the 7 bright).
                static const u32 vga[16] = {0xFF000000, 0xFF800000, 0xFF008000, 0xFF808000, 0xFF000080, 0xFF800080, 0xFF008080, 0xFF808080,
                                            0xFFC0C0C0, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00, 0xFF0000FF, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF};
                pal.push_back(vga[i]);
            }
        }
        const bool cmyk = o.save_as_cmyk && depth >= 24;
        const u32 spp = depth <= 8 ? 1 : cmyk ? 4 : depth == 24 ? 3 : 4;
        const usize rb = depth <= 8 ? (static_cast<usize>(f.width) * depth + 7) / 8 : static_cast<usize>(f.width) * spp;
        std::vector<u8> pix(rb * f.height, 0);
        for (i32 y = 0; y < f.height; ++y) {
            u8* row = pix.data() + rb * y;
            for (i32 x = 0; x < f.width; ++x) {
                const u32 c = f.argb[static_cast<usize>(y) * f.width + x];
                if (depth <= 8) {
                    u32 i;
                    if (depth == 1 && pal.size() == 2 && pal[0] == 0xFF000000u && pal[1] == 0xFFFFFFFFu) {
                        const u32 l = (((c >> 16) & 0xFF) * 299 + ((c >> 8) & 0xFF) * 587 + (c & 0xFF) * 114) / 1000;
                        i = l >= 128 ? 1 : 0;
                    } else {
                        i = nearest(pal, c, false);
                    }
                    if (depth == 8) row[x] = static_cast<u8>(i);
                    else if (depth == 4) row[x >> 1] |= static_cast<u8>((x & 1) ? (i & 15) : ((i & 15) << 4));
                    else row[x >> 3] |= static_cast<u8>((i & 1) << (7 - (x & 7)));
                } else if (cmyk) {
                    const u32 r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF, k = 255 - std::max({r, g, b});
                    const u32 d = 255 - k;
                    auto ch = [&](u32 v) { return d == 0 ? 0u : (d - v) * 255 / d; };
                    row[x * 4] = static_cast<u8>(ch(r)); row[x * 4 + 1] = static_cast<u8>(ch(g)); row[x * 4 + 2] = static_cast<u8>(ch(b)); row[x * 4 + 3] = static_cast<u8>(k);
                } else {
                    row[x * spp] = static_cast<u8>(c >> 16); row[x * spp + 1] = static_cast<u8>(c >> 8); row[x * spp + 2] = static_cast<u8>(c);
                    if (spp == 4) row[x * 4 + 3] = static_cast<u8>(c >> 24);
                }
            }
        }
        const bool lzw = o.tiff_compression != 6;
        const bool predictor = lzw && depth >= 24;
        if (predictor)
            for (i32 y = 0; y < f.height; ++y)
                for (usize i = rb - 1; i >= spp; --i) pix[rb * y + i] = static_cast<u8>(pix[rb * y + i] - pix[rb * y + i - spp]);
        const std::vector<u8> data = lzw ? tiff_lzw(pix) : pix;
        const u32 strip_at = static_cast<u32>(out.size());
        out.insert(out.end(), data.begin(), data.end());
        if (out.size() & 1) out.push_back(0);
        std::vector<TagOut> tags;
        auto add = [&](u16 tag, u16 type, u32 count, std::vector<u8> v) { tags.push_back({tag, type, count, std::move(v)}); };
        add(0xFE, 4, 1, u32v(0));
        add(0x100, 4, 1, u32v(static_cast<u32>(f.width)));
        add(0x101, 4, 1, u32v(static_cast<u32>(f.height)));
        {
            std::vector<u8> v;
            for (u32 i = 0; i < spp; ++i) put16(v, static_cast<u16>(depth <= 8 ? depth : 8));
            add(0x102, 3, spp, v);
        }
        add(0x103, 3, 1, u16v(lzw ? 5 : 1));
        add(0x106, 3, 1, u16v(depth <= 8 ? (depth == 1 && pal == std::vector<u32>{0xFF000000u, 0xFFFFFFFFu} ? 1 : 3) : cmyk ? 5 : 2));
        add(0x111, 4, 1, u32v(strip_at));
        add(0x115, 3, 1, u16v(static_cast<u16>(spp)));
        add(0x116, 4, 1, u32v(static_cast<u32>(f.height)));
        add(0x117, 4, 1, u32v(static_cast<u32>(data.size())));
        add(0x11A, 5, 1, rational(static_cast<u32>(std::lround(f.dpi_x * 1000)), 1000));
        add(0x11B, 5, 1, rational(static_cast<u32>(std::lround(f.dpi_y * 1000)), 1000));
        add(0x11C, 3, 1, u16v(1));
        add(0x128, 3, 1, u16v(2));
        if (predictor) add(0x13D, 3, 1, u16v(2));
        if (depth <= 8 && tags[5].value[0] == 3) {
            std::vector<u8> cm;
            const u32 n = 1u << depth;
            for (int c = 2; c >= 0; --c)
                for (u32 i = 0; i < n; ++i) put16(cm, static_cast<u16>(((i < pal.size() ? pal[i] : 0) >> (c * 8) & 0xFF) * 257));
            add(0x140, 3, 3 * n, cm);
        }
        if (spp == 4 && !cmyk) add(0x152, 3, 1, u16v(2));
        for (const Property& p : f.properties) {
            if (is_structural(p.id) || p.id > 0xFFFF) continue;
            const u32 ts = type_size(p.type);
            if (ts == 0) continue;
            add(static_cast<u16>(p.id), p.type, static_cast<u32>(p.value.size() / ts), p.value);
        }
        std::stable_sort(tags.begin(), tags.end(), [](const TagOut& a, const TagOut& b) { return a.tag < b.tag; });
        const u32 ifd_at = static_cast<u32>(out.size());
        set32(out, link, ifd_at);
        const usize data_at = ifd_at + 2 + tags.size() * 12 + 4;
        std::vector<u8> extra;
        put16(out, static_cast<u16>(tags.size()));
        for (const TagOut& t : tags) {
            put16(out, t.tag); put16(out, t.type); put32(out, t.count);
            if (t.value.size() <= 4) {
                std::vector<u8> v = t.value;
                v.resize(4, 0);
                out.insert(out.end(), v.begin(), v.end());
            } else {
                put32(out, static_cast<u32>(data_at + extra.size()));
                extra.insert(extra.end(), t.value.begin(), t.value.end());
                if (extra.size() & 1) extra.push_back(0);
            }
        }
        link = out.size();
        put32(out, 0);
        out.insert(out.end(), extra.begin(), extra.end());
    }
    return Status::Ok;
}

// ---- JPEG encoder: ImageIO pixels plus an Exif APP1 built from the properties ----------------------------------------

namespace {

std::vector<u8> build_exif(const std::vector<Property>& props) {
    // IFD0 (TIFF tags), the Exif IFD (0x82xx-0xAxxx), the GPS IFD (0x0000-0x001F).
    std::vector<const Property*> ifd0, exif, gps;
    for (const Property& p : props) {
        if (p.id > 0xFFFF || is_structural(p.id) || type_size(p.type) == 0) continue;
        if (p.id <= 0x001F) gps.push_back(&p);
        else if ((p.id >= 0x829A && p.id <= 0x829D) || (p.id >= 0x8822 && p.id <= 0x8830) || (p.id >= 0x9000 && p.id <= 0xA4FF)) exif.push_back(&p);
        else if (p.id < 0x5000 || p.id == 0x8298) ifd0.push_back(&p);
    }
    if (ifd0.empty() && exif.empty() && gps.empty()) return {};
    std::vector<u8> out = {'I', 'I', 42, 0, 8, 0, 0, 0};
    auto by_id = [](const Property* a, const Property* b) { return a->id < b->id; };
    std::sort(ifd0.begin(), ifd0.end(), by_id);
    std::sort(exif.begin(), exif.end(), by_id);
    std::sort(gps.begin(), gps.end(), by_id);
    struct Ent { u16 tag, type; u32 count; std::vector<u8> v; };
    auto write_ifd = [&](std::vector<Ent> ents, std::vector<usize>* ptr_slots) {
        std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.tag < b.tag; });
        const usize at = out.size();
        const usize data_at = at + 2 + ents.size() * 12 + 4;
        std::vector<u8> extra;
        put16(out, static_cast<u16>(ents.size()));
        for (const Ent& e : ents) {
            put16(out, e.tag); put16(out, e.type); put32(out, e.count);
            if (ptr_slots && (e.tag == kExifIfd || e.tag == kGpsIfd)) ptr_slots->push_back(out.size());
            if (e.v.size() <= 4) { auto v = e.v; v.resize(4, 0); out.insert(out.end(), v.begin(), v.end()); }
            else { put32(out, static_cast<u32>(data_at + extra.size())); extra.insert(extra.end(), e.v.begin(), e.v.end()); if (extra.size() & 1) extra.push_back(0); }
        }
        put32(out, 0);
        out.insert(out.end(), extra.begin(), extra.end());
        return at;
    };
    auto ents_of = [](const std::vector<const Property*>& ps) {
        std::vector<Ent> e;
        for (const Property* p : ps) e.push_back({static_cast<u16>(p->id), p->type, static_cast<u32>(p->value.size() / type_size(p->type)), p->value});
        return e;
    };
    std::vector<Ent> e0 = ents_of(ifd0);
    if (!exif.empty()) e0.push_back({kExifIfd, 4, 1, u32v(0)});
    if (!gps.empty()) e0.push_back({kGpsIfd, 4, 1, u32v(0)});
    std::vector<usize> slots;
    write_ifd(e0, &slots);
    usize slot = 0;
    if (!exif.empty()) { const usize at = write_ifd(ents_of(exif), nullptr); set32(out, slots[slot++], static_cast<u32>(at)); }
    if (!gps.empty()) { const usize at = write_ifd(ents_of(gps), nullptr); set32(out, slots[slot++], static_cast<u32>(at)); }
    return out;
}

}  // namespace

Status encode_jpeg(const EncodeFrame& f, const EncodeOptions& o, std::vector<u8>& out) {
    // Default quality: the probe's saved tables are IJG quality-90 tables.
    const i32 q = o.jpeg_quality < 0 ? 90 : std::clamp(o.jpeg_quality, 0, 100);
    std::vector<u32> opaque = f.argb;
    for (u32& c : opaque) c |= kAlphaMask;
    if (!imageio_encode_jpeg(opaque, f.width, f.height, q, f.dpi_x, f.dpi_y, out)) return Status::GenericError;
    const std::vector<u8> exif = build_exif(f.properties);
    if (!exif.empty() && exif.size() + 8 < 65535 && out.size() > 2) {
        usize at = 2;
        if (out.size() > 4 && out[2] == 0xFF && out[3] == 0xE0) at = 4 + be16(out, 4);
        std::vector<u8> seg = {0xFF, 0xE1, static_cast<u8>((exif.size() + 8) >> 8), static_cast<u8>(exif.size() + 8), 'E', 'x', 'i', 'f', 0, 0};
        seg.insert(seg.end(), exif.begin(), exif.end());
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), seg.begin(), seg.end());
    }
    return Status::Ok;
}

}  // namespace vitreo::winapi::gdip::fmt
