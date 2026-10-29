// SPDX-License-Identifier: LGPL-2.1-only
/// @file glu32_image.cpp
/// @brief gluScaleImage's unpack, filter and pack (glu32_image.hpp).

#include "glu32_image.hpp"
#include "winapi/graphics/opengl32/gl_client_memory.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace vitreo::winapi::glu {

namespace {

constexpr u32 kColorIndex = 0x1900, kStencilIndex = 0x1901, kDepthComponent = 0x1902;
constexpr u32 kRed = 0x1903, kGreen = 0x1904, kBlue = 0x1905, kAlpha = 0x1906, kRgb = 0x1907, kRgba = 0x1908;
constexpr u32 kLuminance = 0x1909, kLuminanceAlpha = 0x190A, kBgrExt = 0x80E0, kBgraExt = 0x80E1;
constexpr u32 kByte = 0x1400, kUnsignedByte = 0x1401, kShort = 0x1402, kUnsignedShort = 0x1403;
constexpr u32 kInt = 0x1404, kUnsignedInt = 0x1405, kFloat = 0x1406, kBitmap = 0x1A00;

u32 type_size(u32 type) noexcept {
    switch (type) {
    case kByte: case kUnsignedByte: return 1;
    case kShort: case kUnsignedShort: return 2;
    case kInt: case kUnsignedInt: case kFloat: return 4;
    default: return 0;   // GL_BITMAP: bits
    }
}

struct Layout {
    usize row_bytes{0};      ///< stride between rows
    usize start{0};          ///< byte offset of the first group
    u32 start_bit{0};        ///< GL_BITMAP: bit offset in the first byte
    usize group_bytes{0};
    usize span{0};           ///< bytes from the pointer through the last one the image touches
};

/// The image's layout in checked arithmetic (as opengl32's gl_pixel_layout):
/// nullopt when an offset or the span does not fit 64 bits or the span is
/// larger than any user address space, so no client range can hold it
/// (final review, Finding 2).
std::optional<Layout> layout_of(const PixelModes& modes, i32 width, i32 height, u32 format, u32 type) noexcept {
    Layout l;
    if (width <= 0 || height <= 0) return l;
    const usize comps = format_components(format);
    const usize groups_per_line = modes.row_length > 0 ? static_cast<usize>(modes.row_length)
                                                        : static_cast<usize>(width);
    const usize align = modes.alignment > 0 ? static_cast<usize>(modes.alignment) : 1;
    const usize skip_rows = static_cast<usize>(modes.skip_rows > 0 ? modes.skip_rows : 0);
    const usize skip_pixels = static_cast<usize>(modes.skip_pixels > 0 ? modes.skip_pixels : 0);
    const usize w = static_cast<usize>(width), h = static_cast<usize>(height);
    usize skipped = 0, rows = 0;
    // groups_per_line, w and SKIP_PIXELS are below 2^31 and comps, sizes and
    // the alignment at most 8: those products fit; the row and skip products
    // are checked.
    if (type == kBitmap) {
        l.row_bytes = (groups_per_line * comps + 7) / 8;
        const usize skip_bits = skip_pixels * comps;
        l.start_bit = static_cast<u32>(skip_bits % 8);
        l.row_bytes = (l.row_bytes + align - 1) / align * align;
        const usize last_row = (l.start_bit + w * comps + 7) / 8;
        if (__builtin_mul_overflow(skip_rows, l.row_bytes, &skipped) ||
            __builtin_add_overflow(skipped, skip_bits / 8, &l.start) ||
            __builtin_mul_overflow(h - 1, l.row_bytes, &rows) || __builtin_add_overflow(rows, last_row, &rows) ||
            __builtin_add_overflow(l.start, rows, &l.span)) {
            return std::nullopt;
        }
    } else {
        const usize size = type_size(type);
        l.group_bytes = comps * size;
        l.row_bytes = groups_per_line * l.group_bytes;
        if (size < align) l.row_bytes = (l.row_bytes + align - 1) / align * align;
        if (__builtin_mul_overflow(skip_rows, l.row_bytes, &skipped) ||
            __builtin_add_overflow(skipped, skip_pixels * l.group_bytes, &l.start) ||
            __builtin_mul_overflow(h - 1, l.row_bytes, &rows) ||
            __builtin_add_overflow(rows, w * l.group_bytes, &rows) || __builtin_add_overflow(l.start, rows, &l.span)) {
            return std::nullopt;
        }
    }
    if (l.span > gl::kGlLargestUserAddress) return std::nullopt;
    return l;
}

u32 load(const u8* p, usize size, bool swap) {
    u8 b[4] = {};
    std::memcpy(b, p, size);
    if (swap) {
        for (usize k = 0; k < size / 2; ++k) {
            const u8 t = b[k];
            b[k] = b[size - 1 - k];
            b[size - 1 - k] = t;
        }
    }
    u32 v = 0;
    std::memcpy(&v, b, size);   // little-endian host
    return v;
}

void store(u8* p, usize size, u32 v, bool swap) {
    u8 b[4] = {};
    std::memcpy(b, &v, size);
    if (swap) {
        for (usize k = 0; k < size / 2; ++k) {
            const u8 t = b[k];
            b[k] = b[size - 1 - k];
            b[size - 1 - k] = t;
        }
    }
    std::memcpy(p, b, size);
}

u16 to_u16(u32 type, u32 raw, bool index) noexcept {
    switch (type) {
    case kUnsignedByte: return static_cast<u16>(index ? raw : raw * 257u);
    case kByte: {
        const i32 v = static_cast<i8>(raw);
        return static_cast<u16>(index ? v : v * 516);
    }
    case kUnsignedShort: return static_cast<u16>(raw);
    case kShort: {
        const i32 v = static_cast<i16>(raw);
        return static_cast<u16>(index ? v : v * 2);
    }
    case kUnsignedInt: return static_cast<u16>(index ? raw : raw >> 16);
    case kInt: {
        const i32 v = static_cast<i32>(raw);
        return static_cast<u16>(index ? v : v >> 15);
    }
    case kFloat: {
        float f = 0;
        std::memcpy(&f, &raw, 4);
        const float scaled = index ? f : f * 65535.0f;
        if (!(std::fabs(scaled) < 2147483648.0f)) return 0;   // out of int range (Windows: undefined)
        return static_cast<u16>(static_cast<i32>(scaled));
    }
    default: return 0;
    }
}

u32 from_u16(u32 type, u16 v, bool index) noexcept {
    switch (type) {
    case kUnsignedByte: return index ? static_cast<u8>(v) : static_cast<u32>(v >> 8);
    case kByte: return index ? static_cast<u8>(v) : static_cast<u32>(v >> 9);
    case kUnsignedShort: return v;
    case kShort: return index ? v : static_cast<u32>(v >> 1);
    case kUnsignedInt: return index ? v : static_cast<u32>(v) * 65537u;
    case kInt: return index ? v : (static_cast<u32>(v) * 65537u) >> 1;
    case kFloat: {
        const float f = index ? static_cast<float>(v) : static_cast<float>(v) / 65535.0f;
        u32 bits = 0;
        std::memcpy(&bits, &f, 4);
        return bits;
    }
    default: return 0;
    }
}

/// Where bit @p position of a bitmap row lives (spec 3.6.4): byte position /
/// 8; within it, bit position % 8 counted from the most significant end, or
/// from the least significant end under GL_UNPACK_LSB_FIRST / PACK_LSB_FIRST.
struct BitPosition {
    usize byte;
    u8 mask;
};

BitPosition bitmap_bit(usize position, bool lsb_first) noexcept {
    const u32 within = static_cast<u32>(position % 8);
    const u32 shift = lsb_first ? within : 7 - within;
    return BitPosition{position / 8, static_cast<u8>(1u << shift)};
}

/// The bit a 16-bit component packs to: for a colour, whether it lies in the
/// upper half of the range (above 32767, glu-scale-bitmap-*); for an index,
/// its least significant bit.
bool bitmap_value(u16 component, bool index) noexcept {
    return index ? (component & 1u) != 0 : component >= 0x8000;
}

/// An exact 2:1 reduction in both directions: each output component is the
/// mean of its 2x2 source block rounded half up, ((sum + 2) / 4)
/// (glu-scale-ushort-2x2-0-0-1-2-to-1x1: (0+0+1+2+2)/4 = 1).
void reduce_by_half(u32 comps, i32 width, i32 height, const u16* in, u16* out) {
    const usize row = static_cast<usize>(width) * comps;
    for (i32 oy = 0; oy < height / 2; ++oy) {
        const u16* lower = in + static_cast<usize>(2 * oy) * row;
        const u16* upper = lower + row;
        for (i32 ox = 0; ox < width / 2; ++ox) {
            const usize left = static_cast<usize>(2 * ox) * comps;
            const usize right = left + comps;
            for (u32 k = 0; k < comps; ++k) {
                const u32 sum = u32{lower[left + k]} + lower[right + k] + upper[left + k] + upper[right + k];
                *out++ = static_cast<u16>((sum + 2) / 4);
            }
        }
    }
}

/// The source cells each output pixel's box covers along one axis, with the
/// fraction of each it covers. The box is [c - r/2, c + r/2) around the output
/// centre c (in source units, r the source/output ratio) when shrinking and
/// [c - 0.5, c + 0.5) when not; it is cut at source cell edges, and a cell
/// index outside the image wraps to the other side (glu-scale-lum-ubyte-3x1-to-7x1).
/// Single precision throughout, as glu32 computes it. Output pixel o covers
/// entries first[o] .. first[o + 1] - 1 of cell / part.
struct Coverage {
    HostBuffer<usize> first;
    HostBuffer<i32> cell;
    HostBuffer<float> part;
};

/// Calls @p visit(o, cell, part) for every cell output pixel o's box covers,
/// in order.
template <typename Visit>
void walk_axis(i32 in_size, i32 out_size, Visit&& visit) {
#pragma clang fp contract(off)   // glu32 rounds every product (no fused multiply-add)
    const float ratio = static_cast<float>(in_size) / static_cast<float>(out_size);
    const float reach = in_size > out_size ? ratio / 2 : 0.5f;
    for (i32 o = 0; o < out_size; ++o) {
        const float centre = ratio * (static_cast<float>(o) + 0.5f);
        const float from = centre - reach;
        const float to = centre + reach;
        float at = from;
        i32 edge = static_cast<i32>(std::floor(at));
        while (at < to) {
            const float next = static_cast<float>(edge + 1);
            visit(o, ((edge % in_size) + in_size) % in_size, to < next ? to - at : next - at);
            ++edge;
            at = static_cast<float>(edge);
        }
    }
}

/// The coverage tables of one axis; false when the host has no room for them.
bool axis_coverage(i32 in_size, i32 out_size, Coverage& axis) {
    usize total = 0;
    walk_axis(in_size, out_size, [&total](i32, i32, float) { ++total; });
    if (!axis.first.allocate(static_cast<usize>(out_size) + 1) || !axis.cell.allocate(total) ||
        !axis.part.allocate(total)) {
        return false;
    }
    // Every box covers at least one cell (reach > 0), and the pixels come in
    // order, so pixel o's entries end where pixel o + 1's begin.
    axis.first[0] = 0;   // HostBuffer is not initialised
    usize n = 0;
    walk_axis(in_size, out_size, [&](i32 o, i32 cell, float part) {
        axis.cell[n] = cell;
        axis.part[n] = part;
        axis.first[static_cast<usize>(o) + 1] = ++n;
    });
    return true;
}

}  // namespace

bool legal_format(u32 format) noexcept {
    switch (format) {
    case kColorIndex: case kStencilIndex: case kDepthComponent: case kRed: case kGreen: case kBlue: case kAlpha:
    case kRgb: case kRgba: case kLuminance: case kLuminanceAlpha: case kBgrExt: case kBgraExt:
        return true;
    default:
        return false;
    }
}

bool legal_type(u32 type) noexcept {
    switch (type) {
    case kBitmap: case kByte: case kUnsignedByte: case kShort: case kUnsignedShort: case kInt: case kUnsignedInt:
    case kFloat:
        return true;
    default:
        return false;
    }
}

u32 format_components(u32 format) noexcept {
    switch (format) {
    case kLuminanceAlpha: return 2;
    case kRgb: case kBgrExt: return 3;
    case kRgba: case kBgraExt: return 4;
    default: return 1;
    }
}

bool is_index_format(u32 format) noexcept { return format == kColorIndex || format == kStencilIndex; }

std::optional<usize> component_count(i32 width, i32 height, u32 comps) noexcept {
    if (width < 0 || height < 0) return std::nullopt;
    usize pixels = 0, count = 0;
    if (__builtin_mul_overflow(static_cast<usize>(width), static_cast<usize>(height), &pixels) ||
        __builtin_mul_overflow(pixels, static_cast<usize>(comps), &count) ||
        count > SIZE_MAX / sizeof(u16)) {
        return std::nullopt;
    }
    return count;
}

std::optional<usize> image_span(const PixelModes& modes, i32 width, i32 height, u32 format, u32 type) noexcept {
    const std::optional<Layout> l = layout_of(modes, width, height, format, type);
    if (!l) return std::nullopt;
    return l->span;
}

void unpack_image(const PixelModes& modes, i32 width, i32 height, u32 format, u32 type, const u8* data,
                  u16* out) {
    const u32 comps = format_components(format);
    const bool index = is_index_format(format);
    const std::optional<Layout> checked = layout_of(modes, width, height, format, type);
    if (!checked) return;   // image_span refused it: nothing is read
    const Layout& l = *checked;
    const usize per_row = static_cast<usize>(width) * comps;
    // GL_BITMAP (spec 3.6.4): component j of a row is bit (start_bit + j) of
    // the row, counted from the first byte; a set bit is the largest value
    // (65535, or 1 for an index format: glu-scale-bitmap-*), a clear bit 0.
    const u16 bit_on = index ? u16{1} : u16{0xFFFF};
    for (i32 i = 0; i < height; ++i) {
        const u8* row = data + l.start + static_cast<usize>(i) * l.row_bytes;
        u16* dst = out + static_cast<usize>(i) * per_row;
        if (type == kBitmap) {
            for (usize j = 0; j < per_row; ++j) {
                const BitPosition at = bitmap_bit(l.start_bit + j, modes.lsb_first);
                dst[j] = (row[at.byte] & at.mask) != 0 ? bit_on : u16{0};
            }
            continue;
        }
        const usize size = type_size(type);
        for (usize n = 0; n < per_row; ++n) {
            dst[n] = to_u16(type, load(row + n * size, size, modes.swap_bytes && size > 1), index);
        }
    }
}

void pack_image(const PixelModes& modes, i32 width, i32 height, u32 format, u32 type, const u16* in, u8* data) {
    const u32 comps = format_components(format);
    const bool index = is_index_format(format);
    const std::optional<Layout> checked = layout_of(modes, width, height, format, type);
    if (!checked) return;   // image_span refused it: nothing is written
    const Layout& l = *checked;
    const usize per_row = static_cast<usize>(width) * comps;
    for (i32 i = 0; i < height; ++i) {
        u8* row = data + l.start + static_cast<usize>(i) * l.row_bytes;
        const u16* src = in + static_cast<usize>(i) * per_row;
        if (type == kBitmap) {
            // GL_BITMAP (spec 4.3.2): each component becomes one bit, placed as
            // unpacking reads it; the bits of a byte the image does not cover
            // keep their value (glu-scale-bitmap-*).
            for (usize j = 0; j < per_row; ++j) {
                const BitPosition at = bitmap_bit(l.start_bit + j, modes.lsb_first);
                if (bitmap_value(src[j], index)) {
                    row[at.byte] = static_cast<u8>(row[at.byte] | at.mask);
                } else {
                    row[at.byte] = static_cast<u8>(row[at.byte] & ~at.mask);
                }
            }
            continue;
        }
        const usize size = type_size(type);
        for (usize n = 0; n < per_row; ++n) {
            store(row + n * size, size, from_u16(type, src[n], index), modes.swap_bytes && size > 1);
        }
    }
}

bool box_filter_scale(u32 comps, i32 wi, i32 hi, const u16* in, i32 wo, i32 ho, u16* out) {
#pragma clang fp contract(off)   // glu32 rounds every product (no fused multiply-add)
    if (wi == wo * 2 && hi == ho * 2) {
        reduce_by_half(comps, wi, hi, in, out);
        return true;
    }
    // The box filter: each output component is the coverage-weighted sum of
    // the source components under its box divided by the total coverage,
    // truncated (glu-scale-rgba-ubyte-2x2-to-4x4, -type-ushort-2x1-to-3x1).
    Coverage across, down;
    if (!axis_coverage(wi, wo, across) || !axis_coverage(hi, ho, down)) return false;
    std::array<float, 4> sum{};
    for (i32 oy = 0; oy < ho; ++oy) {
        const usize row_from = down.first[static_cast<usize>(oy)];
        const usize row_to = down.first[static_cast<usize>(oy) + 1];
        for (i32 ox = 0; ox < wo; ++ox) {
            const usize col_from = across.first[static_cast<usize>(ox)];
            const usize col_to = across.first[static_cast<usize>(ox) + 1];
            sum.fill(0.0f);
            float weight = 0.0f;
            for (usize r = row_from; r < row_to; ++r) {
                const usize row_base = static_cast<usize>(down.cell[r]) * static_cast<usize>(wi);
                for (usize c = col_from; c < col_to; ++c) {
                    const float share = across.part[c] * down.part[r];
                    weight += share;
                    const u16* src = in + (row_base + static_cast<usize>(across.cell[c])) * comps;
                    for (u32 k = 0; k < comps; ++k) sum[k] += static_cast<float>(src[k]) * share;
                }
            }
            u16* dst = out + (static_cast<usize>(oy) * static_cast<usize>(wo) + static_cast<usize>(ox)) * comps;
            for (u32 k = 0; k < comps; ++k) {
                const float v = weight > 0.0f ? sum[k] / weight : 0.0f;
                dst[k] = v >= 65535.0f ? u16{65535} : static_cast<u16>(v);
            }
        }
    }
    return true;
}

i64 nearest_power(i32 value) noexcept {
    if (value < 1) return 0;
    // The powers of two either side of the value; the nearer one wins, and
    // at the midpoint (value = 1.5 * below) the larger one (glu-build2d-*:
    // 3 -> 4, 6 -> 8, 12 -> 16).
    const u64 v = static_cast<u64>(value);
    const u64 below = std::bit_floor(v);
    const u64 above = below * 2;
    return static_cast<i64>(v - below < above - v ? below : above);
}

}  // namespace vitreo::winapi::glu
