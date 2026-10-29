// SPDX-License-Identifier: LGPL-2.1-only

/// @file icon_planes.cpp
/// @brief Icon/cursor image decoding, scaling and flattening.

#include "winapi/user32/resources/icon_planes.hpp"

#include "winapi/graphics/gdi32/bitmap_codec.hpp"
#include "winapi/graphics/gdiplus/image_decode.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi {

namespace {

[[nodiscard]] u32 le16(const u8* p) noexcept { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8); }
[[nodiscard]] u32 le32(const u8* p) noexcept {
    return le16(p) | (le16(p + 2) << 16);
}

constexpr u32 kMaxIconDim = 1024;

bool is_png(std::span<const u8> d) {
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return d.size() >= 8 && std::memcmp(d.data(), sig, 8) == 0;
}

std::optional<IconPlanes> decode_png(std::span<const u8> d, bool is_icon) {
    auto image = gdiplus_decode::decode(d);
    if (!image || image->width == 0 || image->height == 0 || image->width > kMaxIconDim ||
        image->height > kMaxIconDim) {
        return std::nullopt;
    }
    IconPlanes p;
    p.is_icon = is_icon;
    p.width = image->width;
    p.height = image->height;
    p.color.resize(static_cast<usize>(p.width) * p.height * 4u);
    p.mask.assign(static_cast<usize>(p.width) * p.height, 0);
    for (usize i = 0; i < static_cast<usize>(p.width) * p.height; ++i) {
        const u8* s = image->bgra.data() + i * 4;
        u8* c = p.color.data() + i * 4;
        c[0] = s[2]; c[1] = s[1]; c[2] = s[0]; c[3] = s[3];
        p.mask[i] = s[3] == 0 ? 1 : 0;
    }
    return p;
}

std::optional<IconPlanes> decode_dib(std::span<const u8> d, bool is_icon) {
    if (d.size() < 40) return std::nullopt;
    const u32 header = le32(d.data());
    if (header < 40 || header > d.size()) return std::nullopt;
    const i32 width = static_cast<i32>(le32(d.data() + 4));
    const i32 height2 = static_cast<i32>(le32(d.data() + 8));
    const u32 bpp = le16(d.data() + 14);
    const u32 compression = le32(d.data() + 16);
    const u32 clr_used = le32(d.data() + 32);
    if (width <= 0 || height2 <= 0 || (height2 & 1) != 0 ||
        static_cast<u32>(width) > kMaxIconDim || static_cast<u32>(height2 / 2) > kMaxIconDim) {
        return std::nullopt;
    }
    if (compression != gdi::kBiRgb) return std::nullopt;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return std::nullopt;
    const u32 w = static_cast<u32>(width);
    const u32 h = static_cast<u32>(height2 / 2);
    gdi::PixelFormat format;
    format.bpp = static_cast<u16>(bpp);
    u32 table = 0;
    if (bpp <= 8) {
        const u32 entries = clr_used != 0 ? std::min<u32>(clr_used, 1u << bpp) : (1u << bpp);
        table = entries * 4u;
        if (header + table > d.size()) return std::nullopt;
        for (u32 i = 0; i < entries; ++i) {
            const u8* q = d.data() + header + i * 4u;
            format.palette.push_back((static_cast<u32>(q[2]) << 16) | (static_cast<u32>(q[1]) << 8) | q[0]);
        }
    }
    const u32 xor_stride = gdi::dib_stride(width, bpp);
    const u32 and_stride = gdi::dib_stride(width, 1);
    const usize xor_off = header + table;
    const usize and_off = xor_off + static_cast<usize>(xor_stride) * h;
    if (and_off + static_cast<usize>(and_stride) * h > d.size()) return std::nullopt;

    IconPlanes p;
    p.is_icon = is_icon;
    p.width = w;
    p.height = h;
    const bool mono = bpp == 1;
    p.mask.assign(static_cast<usize>(w) * h * (mono ? 2u : 1u), 0);
    auto bit = [](const u8* row, u32 x) { return static_cast<u8>((row[x >> 3] >> (7 - (x & 7))) & 1u); };
    for (u32 y = 0; y < h; ++y) {
        // DIB rows run bottom-up.
        const u8* and_row = d.data() + and_off + static_cast<usize>(h - 1 - y) * and_stride;
        const u8* xor_row = d.data() + xor_off + static_cast<usize>(h - 1 - y) * xor_stride;
        for (u32 x = 0; x < w; ++x) p.mask[static_cast<usize>(y) * w + x] = bit(and_row, x);
        if (mono) {
            for (u32 x = 0; x < w; ++x) {
                p.mask[static_cast<usize>(h + y) * w + x] = bit(xor_row, x);
            }
        }
    }
    if (!mono) {
        p.color.resize(static_cast<usize>(w) * h * 4u);
        for (u32 y = 0; y < h; ++y) {
            const u8* xor_row = d.data() + xor_off + static_cast<usize>(h - 1 - y) * xor_stride;
            gdi::decode_row(format, xor_row, p.color.data() + static_cast<usize>(y) * w * 4u,
                            static_cast<i32>(w));
        }
    }
    return p;
}

}  // namespace

bool IconPlanes::has_alpha() const noexcept {
    for (usize i = 3; i < color.size(); i += 4) {
        if (color[i] != 0) return true;
    }
    return false;
}

bool IconPlanes::valid() const noexcept {
    const usize n = static_cast<usize>(width) * height;
    return width != 0 && height != 0 &&
           (color.empty() ? mask.size() == n * 2 : (color.size() == n * 4 && mask.size() == n));
}

std::optional<IconPlanes> decode_icon_image(std::span<const u8> data, bool is_icon, u32 want_w,
                                            u32 want_h) {
    u32 hot_x = 0, hot_y = 0;
    if (!is_icon) {
        // An RT_CURSOR image starts with its LOCALHEADER hotspot.
        if (data.size() < 4) return std::nullopt;
        hot_x = le16(data.data());
        hot_y = le16(data.data() + 2);
        data = data.subspan(4);
    }
    auto planes = is_png(data) ? decode_png(data, is_icon) : decode_dib(data, is_icon);
    if (!planes) return std::nullopt;
    if (is_icon) {
        // Learn (ICONINFO): an icon's hotspot is always its centre.
        planes->hotspot_x = planes->width / 2;
        planes->hotspot_y = planes->height / 2;
    } else {
        planes->hotspot_x = hot_x;
        planes->hotspot_y = hot_y;
    }
    if ((want_w != 0 && want_w != planes->width) || (want_h != 0 && want_h != planes->height)) {
        const u32 w = want_w != 0 ? want_w : planes->width;
        const u32 h = want_h != 0 ? want_h : planes->height;
        const u32 ow = planes->width, oh = planes->height;
        IconPlanes scaled = scale_icon_planes(*planes, w, h);
        if (is_icon) {
            scaled.hotspot_x = w / 2;
            scaled.hotspot_y = h / 2;
        } else {
            scaled.hotspot_x = planes->hotspot_x * w / ow;
            scaled.hotspot_y = planes->hotspot_y * h / oh;
        }
        return scaled;
    }
    return planes;
}

std::optional<IconPlanes> decode_icon_file(std::span<const u8> data, u32 want_w, u32 want_h) {
    if (data.size() < 6 || le16(data.data()) != 0) return std::nullopt;
    const u32 type = le16(data.data() + 2);
    const u32 count = le16(data.data() + 4);
    if ((type != 1 && type != 2) || count == 0 || 6 + count * 16u > data.size()) return std::nullopt;
    // Prefer an exact size (deepest first), then the smallest larger image,
    // then the largest.
    i32 best = -1;
    u32 best_score = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* e = data.data() + 6 + i * 16u;
        const u32 w = e[0] ? e[0] : 256u;
        const u32 h = e[1] ? e[1] : 256u;
        const u32 bpp = type == 1 ? le16(e + 6) : 0;
        u32 score = 0;
        if (want_w == 0 && want_h == 0) {
            score = i == 0 ? 1 : 0;
        } else if (w == want_w && h == want_h) {
            score = 0x100000u + bpp;
        } else if (w >= want_w && h >= want_h) {
            score = 0x80000u - w * h / 4u + bpp;
        } else {
            score = w * h / 4u + bpp / 8u;
        }
        if (best < 0 || score > best_score) {
            best = static_cast<i32>(i);
            best_score = score;
        }
    }
    const u8* e = data.data() + 6 + static_cast<u32>(best) * 16u;
    const u32 size = le32(e + 8);
    const u32 offset = le32(e + 12);
    if (offset > data.size() || size > data.size() - offset) return std::nullopt;
    auto planes = decode_icon_image(data.subspan(offset, size), true, want_w, want_h);
    if (planes && type == 2) {
        planes->is_icon = false;
        const u32 w = e[0] ? e[0] : 256u;
        const u32 h = e[1] ? e[1] : 256u;
        planes->hotspot_x = le16(e + 4) * planes->width / w;
        planes->hotspot_y = le16(e + 6) * planes->height / h;
    }
    return planes;
}

IconPlanes scale_icon_planes(const IconPlanes& in, u32 w, u32 h) {
    IconPlanes out = in;
    out.width = w;
    out.height = h;
    const u32 iw = in.width, ih = in.height;
    const u32 mask_rows = in.monochrome() ? 2u : 1u;
    out.mask.assign(static_cast<usize>(w) * h * mask_rows, 0);
    for (u32 part = 0; part < mask_rows; ++part) {
        for (u32 y = 0; y < h; ++y) {
            const u32 sy = static_cast<u32>((static_cast<u64>(2 * y + 1) * ih) / (2 * h));
            for (u32 x = 0; x < w; ++x) {
                const u32 sx = static_cast<u32>((static_cast<u64>(2 * x + 1) * iw) / (2 * w));
                out.mask[(static_cast<usize>(part) * h + y) * w + x] =
                    in.mask[(static_cast<usize>(part) * ih + sy) * iw + sx];
            }
        }
    }
    if (!in.monochrome()) {
        out.color.assign(static_cast<usize>(w) * h * 4u, 0);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                u8* d = out.color.data() + (static_cast<usize>(y) * w + x) * 4u;
                if (w <= iw && h <= ih) {
                    // Shrinking averages the covered block (s3gdi_icon_probe
                    // "copied-dib-2x2").
                    const u32 x0 = x * iw / w, x1 = std::max(x0 + 1, (x + 1) * iw / w);
                    const u32 y0 = y * ih / h, y1 = std::max(y0 + 1, (y + 1) * ih / h);
                    u32 sum[4] = {0, 0, 0, 0};
                    u32 n = 0;
                    for (u32 sy = y0; sy < y1; ++sy) {
                        for (u32 sx = x0; sx < x1; ++sx) {
                            const u8* s = in.color.data() + (static_cast<usize>(sy) * iw + sx) * 4u;
                            for (u32 c = 0; c < 4; ++c) sum[c] += s[c];
                            ++n;
                        }
                    }
                    for (u32 c = 0; c < 4; ++c) d[c] = static_cast<u8>(sum[c] / n);
                } else {
                    // Growing interpolates between pixel centres.
                    const double fx = std::clamp((x + 0.5) * iw / w - 0.5, 0.0, iw - 1.0);
                    const double fy = std::clamp((y + 0.5) * ih / h - 0.5, 0.0, ih - 1.0);
                    const u32 x0 = static_cast<u32>(fx), y0 = static_cast<u32>(fy);
                    const u32 x1 = std::min(x0 + 1, iw - 1), y1 = std::min(y0 + 1, ih - 1);
                    const double ax = fx - x0, ay = fy - y0;
                    for (u32 c = 0; c < 4; ++c) {
                        auto at = [&](u32 sx, u32 sy) {
                            return static_cast<double>(in.color[(static_cast<usize>(sy) * iw + sx) * 4u + c]);
                        };
                        const double v = (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) +
                                         (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
                        d[c] = static_cast<u8>(std::clamp(v + 0.5, 0.0, 255.0));
                    }
                }
            }
        }
    }
    return out;
}

IconPlanes icon_planes_from_rgba(u32 width, u32 height, std::span<const u8> rgba) {
    IconPlanes p;
    p.width = width;
    p.height = height;
    const usize n = static_cast<usize>(width) * height;
    p.color.assign(rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(std::min(n * 4u, rgba.size())));
    p.color.resize(n * 4u, 0);
    p.mask.resize(n);
    for (usize i = 0; i < n; ++i) p.mask[i] = p.color[i * 4 + 3] == 0 ? 1 : 0;
    return p;
}

std::vector<u8> icon_display_rgba(const IconPlanes& p) {
    const usize n = static_cast<usize>(p.width) * p.height;
    std::vector<u8> out(n * 4u, 0);
    if (!p.monochrome() && p.has_alpha()) {
        std::memcpy(out.data(), p.color.data(), n * 4u);
        return out;
    }
    for (usize i = 0; i < n; ++i) {
        u8* d = out.data() + i * 4u;
        if (p.mask[i] != 0) continue;  // AND=1: transparent (or an inverting pixel)
        if (p.monochrome()) {
            const u8 v = p.mask[n + i] ? 0xFF : 0x00;
            d[0] = d[1] = d[2] = v;
        } else {
            std::memcpy(d, p.color.data() + i * 4u, 3);
        }
        d[3] = 0xFF;
    }
    return out;
}

}  // namespace vitreo::winapi
