// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_pixels.cpp
/// @brief Pixel-rectangle unpacking, pixel transfer and packing (OpenGL 1.1
///        spec 3.6.1, 3.6.3, 3.6.4, 4.3.2; gl_pixels.hpp).

#include "gl_pixels.hpp"
#include "gl_client_memory.hpp"
#include "gl_commands.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vitreo::winapi::gl {

bool gl_pixel_type_valid(u32 type, bool allow_bitmap) noexcept {
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: case GL_UNSIGNED_SHORT: case GL_SHORT:
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT:
        return true;
    case GL_BITMAP: return allow_bitmap;
    default: return false;
    }
}

u32 gl_pixel_format_elements(u32 format) noexcept {
    switch (format) {
    case GL_COLOR_INDEX: case GL_STENCIL_INDEX: case GL_DEPTH_COMPONENT:
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE:
        return 1;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_RGB: case GL_BGR_EXT: return 3;    // GL_EXT_bgra
    case GL_RGBA: case GL_BGRA_EXT: return 4;
    default: return 0;
    }
}

u32 gl_pixel_type_bytes(u32 type) noexcept {
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return 1;
    case GL_UNSIGNED_SHORT: case GL_SHORT: return 2;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4;
    default: return 0;
    }
}

namespace {

/// a * b + c in usize; false on overflow.
bool mul_add(usize a, usize b, usize c, usize& out) noexcept {
    usize p = 0;
    return !__builtin_mul_overflow(a, b, &p) && !__builtin_add_overflow(p, c, &out);
}

}  // namespace

GlPixelLayout gl_pixel_layout(const GlPixelStoreModes& modes, u32 format, u32 type, i32 width,
                              i32 height) noexcept {
    GlPixelLayout out;
    if (width <= 0 || height <= 0) return out;
    GlPixelLayout invalid;
    invalid.valid = false;
    // Every offset and size is checked arithmetic: ROW_LENGTH, SKIP_ROWS and
    // SKIP_PIXELS are any non-negative int, and their products with the row
    // and element sizes need more than 64 bits (final review, Finding 1).
    const usize n = gl_pixel_format_elements(format);
    const usize a = static_cast<usize>(std::max(modes.alignment, 1));
    const usize l = modes.row_length > 0 ? static_cast<usize>(modes.row_length) : static_cast<usize>(width);
    const usize w = static_cast<usize>(width), h = static_cast<usize>(height);
    const usize skip_rows = static_cast<usize>(std::max(modes.skip_rows, 0));
    const usize skip_pixels = static_cast<usize>(std::max(modes.skip_pixels, 0));
    if (type == GL_BITMAP) {
        // Eq. 3.10: k = a ceil(n l / 8a) ubytes per row; SKIP_ROWS rows, then
        // SKIP_PIXELS bits are skipped. n l, w and SKIP_PIXELS are below 2^33,
        // so the bit counts themselves fit.
        const usize k = a * ((n * l + 8 * a - 1) / (8 * a));
        out.row_bytes = k;
        out.skip_bits = static_cast<u32>(skip_pixels);
        const usize last_row = (skip_pixels + n * w + 7) / 8;
        usize rows = 0;
        if (!mul_add(skip_rows, k, 0, out.first_byte) || !mul_add(h - 1, k, last_row, rows) ||
            __builtin_add_overflow(out.first_byte, rows, &out.span_bytes)) {
            return invalid;
        }
    } else {
        const usize s = gl_pixel_type_bytes(type);
        if (s == 0 || n == 0) return out;
        // Eq. 3.9: k = n l when s >= a, else (a / s) ceil(s n l / a) elements
        // (n l < 2^33 and s, a <= 8: these fit).
        const usize k = s >= a ? n * l : (a / s) * ((s * n * l + a - 1) / a);
        out.row_bytes = k * s;
        usize skipped = 0, rows = 0;
        if (!mul_add(skip_rows, k, skip_pixels * n, skipped) || !mul_add(skipped, s, 0, out.first_byte) ||
            !mul_add(h - 1, out.row_bytes, w * n * s, rows) ||
            __builtin_add_overflow(out.first_byte, rows, &out.span_bytes)) {
            return invalid;
        }
    }
    // Nothing larger than the largest user address space is client memory.
    if (out.span_bytes > kGlLargestUserAddress) return invalid;
    return out;
}

bool gl_pixel_region_layout(const GlPixelStoreModes& modes, u32 format, u32 type, i32 width, i32 height, i32 x,
                            i32 y, GlPixelStoreModes& region_modes, usize& offset) noexcept {
    const GlPixelLayout full = gl_pixel_layout(modes, format, type, width, height);
    if (!full.valid || x < 0 || y < 0 || x >= width || y >= height) return false;
    region_modes = modes;
    region_modes.row_length = modes.row_length > 0 ? modes.row_length : width;
    region_modes.skip_rows = 0;
    usize column = 0;
    if (type == GL_BITMAP) {
        // The region's first bit lies SKIP_PIXELS + x bits into its row.
        const usize bit = static_cast<usize>(full.skip_bits) + static_cast<usize>(x);
        region_modes.skip_pixels = static_cast<i32>(bit % 8);
        column = bit / 8;
    } else {
        region_modes.skip_pixels = 0;
        column = static_cast<usize>(x) * gl_pixel_format_elements(format) * gl_pixel_type_bytes(type);
    }
    // Inside the full image's span, which is valid: no overflow.
    offset = full.first_byte + static_cast<usize>(y) * full.row_bytes + column;
    return true;
}

namespace {

/// One element at @p p, its bytes reordered per Table 3.6 when SWAP_BYTES.
double element(const u8* p, u32 type, bool swap, bool component) noexcept {
    u8 b[4] = {};
    const u32 size = gl_pixel_type_bytes(type);
    std::memcpy(b, p, size);
    if (swap) std::reverse(b, b + size);
    switch (type) {
    case GL_UNSIGNED_BYTE: return component ? gl_ubyte_to_float(b[0]) : b[0];
    case GL_BYTE: {
        const i8 v = static_cast<i8>(b[0]);
        return component ? gl_byte_to_float(v) : v;
    }
    case GL_UNSIGNED_SHORT: {
        u16 v;
        std::memcpy(&v, b, 2);
        return component ? gl_ushort_to_float(v) : v;
    }
    case GL_SHORT: {
        i16 v;
        std::memcpy(&v, b, 2);
        return component ? gl_short_to_float(v) : v;
    }
    case GL_UNSIGNED_INT: {
        u32 v;
        std::memcpy(&v, b, 4);
        return component ? gl_uint_to_float(v) : v;
    }
    case GL_INT: {
        i32 v;
        std::memcpy(&v, b, 4);
        return component ? gl_int_to_float(v) : v;
    }
    case GL_FLOAT: {
        float v;
        std::memcpy(&v, b, 4);
        return v;
    }
    default: return 0.0;
    }
}

/// The value of a 2^n-entry index map at round(index) & (2^n - 1).
float index_map(const std::vector<float>& map, double index) noexcept {
    if (map.empty()) return 0.0f;
    const i64 rounded = static_cast<i64>(std::floor(index + 0.5));
    return map[static_cast<usize>(rounded & static_cast<i64>(map.size() - 1))];
}

const std::vector<float>& pixel_map(const GlContext& ctx, u32 map) noexcept {
    return ctx.pixel_maps[map - GL_PIXEL_MAP_I_TO_I];
}

/// The index element of group (x, y) of a one-element index format: a bit
/// of a BITMAP image (spec 3.6.4: MSB first unless LSB_FIRST, after
/// SKIP_PIXELS bits) or the element as a fixed-point value (indices are not
/// converted to floating point; integer types keep no fraction bits).
double read_index(std::span<const u8> data, const GlPixelLayout& layout, const GlPixelStoreModes& modes, u32 type,
                  i32 x, i32 y) noexcept {
    if (type == GL_BITMAP) return gl_bitmap_bit(data, layout, modes, x, y) ? 1.0 : 0.0;
    const usize at = layout.first_byte + static_cast<usize>(y) * layout.row_bytes +
                     static_cast<usize>(x) * gl_pixel_type_bytes(type);
    VITREO_ASSERT(at + gl_pixel_type_bytes(type) <= data.size());
    const double idx = element(&data[at], type, modes.swap_bytes, false);
    return type != GL_FLOAT ? std::trunc(idx) : idx;
}

}  // namespace

bool gl_bitmap_bit(std::span<const u8> data, const GlPixelLayout& layout, const GlPixelStoreModes& modes, i32 x,
                   i32 y) noexcept {
    const usize bit = layout.skip_bits + static_cast<usize>(x);
    const usize at = layout.first_byte + static_cast<usize>(y) * layout.row_bytes + bit / 8;
    VITREO_ASSERT(at < data.size());
    const u8 byte = data[at];
    const u32 shift = modes.lsb_first ? static_cast<u32>(bit % 8) : 7u - static_cast<u32>(bit % 8);
    return ((byte >> shift) & 1u) != 0;
}

double gl_index_arithmetic(const GlContext& ctx, double idx, bool integer) noexcept {
    const GlPixelModeState& p = ctx.state.pixel_mode;
    // Shift by |INDEX_SHIFT| (zero-filled), then INDEX_OFFSET.
    if (p.index_shift > 0) {
        idx = std::ldexp(idx, p.index_shift);
    } else if (p.index_shift < 0) {
        idx = std::ldexp(idx, p.index_shift);
        if (integer) idx = std::floor(idx);
    }
    return idx + p.index_offset;
}

double gl_transfer_stencil(const GlContext& ctx, double index, bool integer) noexcept {
    double idx = gl_index_arithmetic(ctx, index, integer);
    if (ctx.state.pixel_mode.map_stencil) {
        // The table value replaces the index, rounded to fixed point.
        idx = std::floor(static_cast<double>(index_map(pixel_map(ctx, GL_PIXEL_MAP_S_TO_S), idx)) + 0.5);
    }
    return idx;
}

float gl_transfer_depth(const GlContext& ctx, float depth) noexcept {
    const GlPixelModeState& p = ctx.state.pixel_mode;
    return depth * p.depth_scale + p.depth_bias;
}

void gl_unpack_stencil(const GlContext& ctx, const GlPixelStoreModes& modes, u32 type, i32 width, i32 height,
                       std::span<const u8> data, std::vector<double>& out) {
    out.clear();
    if (width <= 0 || height <= 0) return;
    const GlPixelLayout layout = gl_pixel_layout(modes, GL_STENCIL_INDEX, type, width, height);
    if (!layout.valid || data.size() < layout.span_bytes) return;
    if (!gl_try_reserve(out, static_cast<usize>(width) * static_cast<usize>(height))) return;   // GL_OUT_OF_MEMORY
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            out.push_back(gl_transfer_stencil(ctx, read_index(data, layout, modes, type, x, y), type != GL_FLOAT));
        }
    }
}

void gl_unpack_color_indices(const GlContext& ctx, const GlPixelStoreModes& modes, u32 type, i32 width, i32 height,
                             std::span<const u8> data, std::vector<double>& out) {
    out.clear();
    if (width <= 0 || height <= 0) return;
    const GlPixelLayout layout = gl_pixel_layout(modes, GL_COLOR_INDEX, type, width, height);
    if (!layout.valid || data.size() < layout.span_bytes) return;
    if (!gl_try_reserve(out, static_cast<usize>(width) * static_cast<usize>(height))) return;   // GL_OUT_OF_MEMORY
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            double idx = gl_index_arithmetic(ctx, read_index(data, layout, modes, type, x, y), type != GL_FLOAT);
            if (ctx.state.pixel_mode.map_color) idx = index_map(pixel_map(ctx, GL_PIXEL_MAP_I_TO_I), idx);
            out.push_back(idx);
        }
    }
}

void gl_unpack_depth(const GlContext& ctx, const GlPixelStoreModes& modes, u32 type, i32 width, i32 height,
                     std::span<const u8> data, std::vector<float>& out) {
    out.clear();
    if (width <= 0 || height <= 0 || type == GL_BITMAP) return;
    const GlPixelLayout layout = gl_pixel_layout(modes, GL_DEPTH_COMPONENT, type, width, height);
    if (!layout.valid || data.size() < layout.span_bytes) return;
    const u32 s = gl_pixel_type_bytes(type);
    if (!gl_try_reserve(out, static_cast<usize>(width) * static_cast<usize>(height))) return;   // GL_OUT_OF_MEMORY
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            const usize at = layout.first_byte + static_cast<usize>(y) * layout.row_bytes + static_cast<usize>(x) * s;
            VITREO_ASSERT(at + s <= data.size());
            const float d = static_cast<float>(element(&data[at], type, modes.swap_bytes, true));
            // Final conversion: clamped to [0, 1], then the depth buffer's
            // fixed point (the backend attachment's).
            out.push_back(std::clamp(gl_transfer_depth(ctx, d), 0.0f, 1.0f));
        }
    }
}

void gl_transfer_rgba(const GlContext& ctx, GlRgba& c) noexcept {
    const GlPixelModeState& p = ctx.state.pixel_mode;
    // Arithmetic on components.
    for (usize k = 0; k < 4; ++k) c[k] = c[k] * p.scale[k] + p.bias[k];
    // RGBA to RGBA lookup.
    if (p.map_color) {
        static constexpr u32 kMaps[] = {GL_PIXEL_MAP_R_TO_R, GL_PIXEL_MAP_G_TO_G, GL_PIXEL_MAP_B_TO_B,
                                        GL_PIXEL_MAP_A_TO_A};
        for (usize k = 0; k < 4; ++k) {
            const std::vector<float>& map = pixel_map(ctx, kMaps[k]);
            if (map.empty()) continue;
            const double v = std::clamp(static_cast<double>(c[k]), 0.0, 1.0);
            const usize address = static_cast<usize>(std::floor(v * static_cast<double>(map.size() - 1) + 0.5));
            c[k] = map[std::min(address, map.size() - 1)];
        }
    }
}

void gl_unpack_rgba(const GlContext& ctx, const GlPixelStoreModes& modes, u32 format, u32 type, i32 width,
                    i32 height, std::span<const u8> data, std::vector<GlRgba>& out) {
    out.clear();
    if (width <= 0 || height <= 0) return;
    const GlPixelLayout layout = gl_pixel_layout(modes, format, type, width, height);
    if (!layout.valid || data.size() < layout.span_bytes) return;
    const u32 n = gl_pixel_format_elements(format);
    const u32 s = gl_pixel_type_bytes(type);
    if (!gl_try_reserve(out, static_cast<usize>(width) * static_cast<usize>(height))) return;   // GL_OUT_OF_MEMORY
    const bool index = format == GL_COLOR_INDEX;
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            GlRgba c{0.0f, 0.0f, 0.0f, 1.0f};
            if (index) {
                const double idx =
                    gl_index_arithmetic(ctx, read_index(data, layout, modes, type, x, y), type != GL_FLOAT);
                // Color index lookup: RGBA groups are required (texture images).
                c = {index_map(pixel_map(ctx, GL_PIXEL_MAP_I_TO_R), idx),
                     index_map(pixel_map(ctx, GL_PIXEL_MAP_I_TO_G), idx),
                     index_map(pixel_map(ctx, GL_PIXEL_MAP_I_TO_B), idx),
                     index_map(pixel_map(ctx, GL_PIXEL_MAP_I_TO_A), idx)};
                out.push_back(c);
                continue;
            }
            const usize at = layout.first_byte + static_cast<usize>(y) * layout.row_bytes +
                             static_cast<usize>(x) * n * s;
            std::array<float, 4> e{};
            VITREO_ASSERT(at + n * s <= data.size());
            for (u32 k = 0; k < n; ++k) {
                e[k] = static_cast<float>(element(&data[at + k * s], type, modes.swap_bytes, true));
            }
            // Conversion to RGB, then the final expansion to RGBA.
            switch (format) {
            case GL_RED: c[0] = e[0]; break;
            case GL_GREEN: c[1] = e[0]; break;
            case GL_BLUE: c[2] = e[0]; break;
            case GL_ALPHA: c[3] = e[0]; break;
            case GL_LUMINANCE: c[0] = c[1] = c[2] = e[0]; break;
            case GL_LUMINANCE_ALPHA:
                c[0] = c[1] = c[2] = e[0];
                c[3] = e[1];
                break;
            case GL_RGB: c = {e[0], e[1], e[2], 1.0f}; break;
            case GL_RGBA: c = {e[0], e[1], e[2], e[3]}; break;
            // GL_EXT_bgra: the elements are blue, green, red (, alpha).
            case GL_BGR_EXT: c = {e[2], e[1], e[0], 1.0f}; break;
            case GL_BGRA_EXT: c = {e[2], e[1], e[0], e[3]}; break;
            default: break;
            }
            gl_transfer_rgba(ctx, c);
            out.push_back(c);
        }
    }
}

namespace {

/// Table 4.7: a [0, 1] component as an element of @p type, rounded to nearest.
void store_element(u8* p, u32 type, bool swap, double c) noexcept {
    u8 b[4] = {};
    const u32 size = gl_pixel_type_bytes(type);
    const auto as_unsigned = [](double v, double max) { return std::floor(v * max + 0.5); };
    const auto as_signed = [](double v, double max) { return std::floor(((max * v) - 1.0) / 2.0 + 0.5); };
    switch (type) {
    case GL_UNSIGNED_BYTE: b[0] = static_cast<u8>(as_unsigned(c, 255.0)); break;
    case GL_BYTE: b[0] = static_cast<u8>(static_cast<i8>(as_signed(c, 255.0))); break;
    case GL_UNSIGNED_SHORT: {
        const u16 v = static_cast<u16>(as_unsigned(c, 65535.0));
        std::memcpy(b, &v, 2);
        break;
    }
    case GL_SHORT: {
        const i16 v = static_cast<i16>(as_signed(c, 65535.0));
        std::memcpy(b, &v, 2);
        break;
    }
    case GL_UNSIGNED_INT: {
        const u32 v = static_cast<u32>(as_unsigned(c, 4294967295.0));
        std::memcpy(b, &v, 4);
        break;
    }
    case GL_INT: {
        const i32 v = static_cast<i32>(as_signed(c, 4294967295.0));
        std::memcpy(b, &v, 4);
        break;
    }
    case GL_FLOAT: {
        const float v = static_cast<float>(c);
        std::memcpy(b, &v, 4);
        break;
    }
    default: return;
    }
    if (swap) std::reverse(b, b + size);
    std::memcpy(p, b, size);
}

}  // namespace

void gl_pack_rgba(const GlPixelStoreModes& modes, u32 format, u32 type, i32 width, i32 height,
                  std::span<const GlRgba> groups, std::span<u8> out, std::span<const u8> present) {
    if (width <= 0 || height <= 0) return;
    const GlPixelLayout layout = gl_pixel_layout(modes, format, type, width, height);
    if (!layout.valid || out.size() < layout.span_bytes || type == GL_BITMAP) return;
    if (groups.size() < static_cast<usize>(width) * static_cast<usize>(height)) return;
    const u32 n = gl_pixel_format_elements(format);
    const u32 s = gl_pixel_type_bytes(type);
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            const usize g = static_cast<usize>(y) * static_cast<usize>(width) + static_cast<usize>(x);
            if (!present.empty() && (g >= present.size() || present[g] == 0)) continue;
            const GlRgba& c = groups[g];
            std::array<double, 4> e{};
            const double l = static_cast<double>(c[0]) + c[1] + c[2];
            switch (format) {
            case GL_RED: e[0] = c[0]; break;
            case GL_GREEN: e[0] = c[1]; break;
            case GL_BLUE: e[0] = c[2]; break;
            case GL_ALPHA: e[0] = c[3]; break;
            case GL_DEPTH_COMPONENT: e[0] = c[0]; break;
            case GL_LUMINANCE: e[0] = l; break;
            case GL_LUMINANCE_ALPHA: e = {l, c[3], 0, 0}; break;
            case GL_RGB: e = {c[0], c[1], c[2], 0}; break;
            case GL_RGBA: e = {c[0], c[1], c[2], c[3]}; break;
            case GL_BGR_EXT: e = {c[2], c[1], c[0], 0}; break;
            case GL_BGRA_EXT: e = {c[2], c[1], c[0], c[3]}; break;
            default: return;
            }
            const usize at = layout.first_byte + static_cast<usize>(y) * layout.row_bytes +
                             static_cast<usize>(x) * n * s;
            VITREO_ASSERT(at + n * s <= out.size());
            for (u32 k = 0; k < n; ++k) {
                store_element(&out[at + k * s], type, modes.swap_bytes, std::clamp(e[k], 0.0, 1.0));
            }
        }
    }
}

void gl_pack_indices(const GlPixelStoreModes& modes, u32 type, i32 width, i32 height, std::span<const double> indices,
                     std::span<u8> out, std::span<const u8> present) {
    if (width <= 0 || height <= 0) return;
    const GlPixelLayout layout = gl_pixel_layout(modes, GL_STENCIL_INDEX, type, width, height);
    if (!layout.valid || out.size() < layout.span_bytes) return;
    if (indices.size() < static_cast<usize>(width) * static_cast<usize>(height)) return;
    const u32 s = gl_pixel_type_bytes(type);
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            const usize g = static_cast<usize>(y) * static_cast<usize>(width) + static_cast<usize>(x);
            if (!present.empty() && (g >= present.size() || present[g] == 0)) continue;
            const double idx = indices[g];
            if (type == GL_BITMAP) {
                const usize bit = layout.skip_bits + static_cast<usize>(x);
                const usize at = layout.first_byte + static_cast<usize>(y) * layout.row_bytes + bit / 8;
                VITREO_ASSERT(at < out.size());
                u8& byte = out[at];
                const u32 shift = modes.lsb_first ? static_cast<u32>(bit % 8) : 7u - static_cast<u32>(bit % 8);
                const u8 mask = static_cast<u8>(1u << shift);
                byte = (static_cast<i64>(std::floor(idx)) & 1) != 0 ? static_cast<u8>(byte | mask)
                                                                     : static_cast<u8>(byte & ~mask);
                continue;
            }
            u8 b[4] = {};
            if (type == GL_FLOAT) {
                const float v = static_cast<float>(idx);
                std::memcpy(b, &v, 4);
            } else {
                // Table 4.6: the index masked to the type's magnitude bits.
                u64 mask = 0;
                switch (type) {
                case GL_UNSIGNED_BYTE: mask = 0xFFu; break;
                case GL_BYTE: mask = 0x7Fu; break;
                case GL_UNSIGNED_SHORT: mask = 0xFFFFu; break;
                case GL_SHORT: mask = 0x7FFFu; break;
                case GL_UNSIGNED_INT: mask = 0xFFFFFFFFu; break;
                case GL_INT: mask = 0x7FFFFFFFu; break;
                default: return;
                }
                const u32 v = static_cast<u32>(static_cast<u64>(static_cast<i64>(std::floor(idx))) & mask);
                std::memcpy(b, &v, s);   // little-endian: the low s bytes
            }
            if (modes.swap_bytes) std::reverse(b, b + s);
            const usize at = layout.first_byte + static_cast<usize>(y) * layout.row_bytes + static_cast<usize>(x) * s;
            VITREO_ASSERT(at + s <= out.size());
            std::memcpy(&out[at], b, s);
        }
    }
}

}  // namespace vitreo::winapi::gl
