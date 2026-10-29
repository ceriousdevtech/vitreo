// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_texture.cpp
/// @brief OpenGL 1.1 texturing (spec 3.8, 2.10.4, 6.1.3, 6.1.4): texture
///        images (glTexImage*, glTexSubImage*, glCopyTex*, glGetTexImage),
///        texture objects (glGen/Delete/Bind/IsTexture, residency, priority),
///        texture parameters, the texture environment, texture coordinate
///        generation, the internal-format table and the backend storage a
///        draw samples (gl_texture.hpp).
///
/// Every access to a texture object holds its share group's lock
/// (GlShareGroup), because contexts sharing the group may be current on
/// other threads.

#include "gl_cmd_internal.hpp"
#include "gl_drawable.hpp"
#include "gl_texture.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/backend_texture_2d.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vitreo::winapi::gl {

using detail::as_enum;
using detail::clamp01;
using detail::color_component;

// ---- Internal formats (Tables 3.14-3.15) ----------------------------------------

namespace {

constexpr u8 kZero = 4, kOne = 5;
constexpr std::array<u8, 4> kSwizzleAlpha{kZero, kZero, kZero, 3};
constexpr std::array<u8, 4> kSwizzleLuminance{0, 0, 0, kOne};
constexpr std::array<u8, 4> kSwizzleLuminanceAlpha{0, 0, 0, 3};
constexpr std::array<u8, 4> kSwizzleIntensity{0, 0, 0, 0};
constexpr std::array<u8, 4> kSwizzleRgb{0, 1, 2, kOne};
constexpr std::array<u8, 4> kSwizzleRgba{0, 1, 2, 3};

struct FormatRow {
    u32 internal_format;
    u32 base;
    u8 r, g, b, a, l, i;   // Table 3.15 bits; 0 for base / legacy formats
};

// Table 3.14 (base internal formats), the GL 1.0 component counts 1-4
// (3.8.1: LUMINANCE, LUMINANCE_ALPHA, RGB, RGBA) and Table 3.15 (sized).
constexpr FormatRow kFormats[] = {
    {GL_ALPHA, GL_ALPHA, 0, 0, 0, 0, 0, 0},
    {GL_LUMINANCE, GL_LUMINANCE, 0, 0, 0, 0, 0, 0},
    {GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, 0, 0, 0, 0, 0, 0},
    {GL_INTENSITY, GL_INTENSITY, 0, 0, 0, 0, 0, 0},
    {GL_RGB, GL_RGB, 0, 0, 0, 0, 0, 0},
    {GL_RGBA, GL_RGBA, 0, 0, 0, 0, 0, 0},
    {1, GL_LUMINANCE, 0, 0, 0, 0, 0, 0},
    {2, GL_LUMINANCE_ALPHA, 0, 0, 0, 0, 0, 0},
    {3, GL_RGB, 0, 0, 0, 0, 0, 0},
    {4, GL_RGBA, 0, 0, 0, 0, 0, 0},
    {GL_ALPHA4, GL_ALPHA, 0, 0, 0, 4, 0, 0},
    {GL_ALPHA8, GL_ALPHA, 0, 0, 0, 8, 0, 0},
    {GL_ALPHA12, GL_ALPHA, 0, 0, 0, 12, 0, 0},
    {GL_ALPHA16, GL_ALPHA, 0, 0, 0, 16, 0, 0},
    {GL_LUMINANCE4, GL_LUMINANCE, 0, 0, 0, 0, 4, 0},
    {GL_LUMINANCE8, GL_LUMINANCE, 0, 0, 0, 0, 8, 0},
    {GL_LUMINANCE12, GL_LUMINANCE, 0, 0, 0, 0, 12, 0},
    {GL_LUMINANCE16, GL_LUMINANCE, 0, 0, 0, 0, 16, 0},
    {GL_LUMINANCE4_ALPHA4, GL_LUMINANCE_ALPHA, 0, 0, 0, 4, 4, 0},
    {GL_LUMINANCE6_ALPHA2, GL_LUMINANCE_ALPHA, 0, 0, 0, 2, 6, 0},
    {GL_LUMINANCE8_ALPHA8, GL_LUMINANCE_ALPHA, 0, 0, 0, 8, 8, 0},
    {GL_LUMINANCE12_ALPHA4, GL_LUMINANCE_ALPHA, 0, 0, 0, 4, 12, 0},
    {GL_LUMINANCE12_ALPHA12, GL_LUMINANCE_ALPHA, 0, 0, 0, 12, 12, 0},
    {GL_LUMINANCE16_ALPHA16, GL_LUMINANCE_ALPHA, 0, 0, 0, 16, 16, 0},
    {GL_INTENSITY4, GL_INTENSITY, 0, 0, 0, 0, 0, 4},
    {GL_INTENSITY8, GL_INTENSITY, 0, 0, 0, 0, 0, 8},
    {GL_INTENSITY12, GL_INTENSITY, 0, 0, 0, 0, 0, 12},
    {GL_INTENSITY16, GL_INTENSITY, 0, 0, 0, 0, 0, 16},
    {GL_R3_G3_B2, GL_RGB, 3, 3, 2, 0, 0, 0},
    {GL_RGB4, GL_RGB, 4, 4, 4, 0, 0, 0},
    {GL_RGB5, GL_RGB, 5, 5, 5, 0, 0, 0},
    {GL_RGB8, GL_RGB, 8, 8, 8, 0, 0, 0},
    {GL_RGB10, GL_RGB, 10, 10, 10, 0, 0, 0},
    {GL_RGB12, GL_RGB, 12, 12, 12, 0, 0, 0},
    {GL_RGB16, GL_RGB, 16, 16, 16, 0, 0, 0},
    {GL_RGBA2, GL_RGBA, 2, 2, 2, 2, 0, 0},
    {GL_RGBA4, GL_RGBA, 4, 4, 4, 4, 0, 0},
    {GL_RGB5_A1, GL_RGBA, 5, 5, 5, 1, 0, 0},
    {GL_RGBA8, GL_RGBA, 8, 8, 8, 8, 0, 0},
    {GL_RGB10_A2, GL_RGBA, 10, 10, 10, 2, 0, 0},
    {GL_RGBA12, GL_RGBA, 12, 12, 12, 12, 0, 0},
    {GL_RGBA16, GL_RGBA, 16, 16, 16, 16, 0, 0},
};

std::array<u8, 4> swizzle_of(u32 base) noexcept {
    switch (base) {
    case GL_ALPHA: return kSwizzleAlpha;
    case GL_LUMINANCE: return kSwizzleLuminance;
    case GL_LUMINANCE_ALPHA: return kSwizzleLuminanceAlpha;
    case GL_INTENSITY: return kSwizzleIntensity;
    case GL_RGB: return kSwizzleRgb;
    default: return kSwizzleRgba;
    }
}

}  // namespace

std::optional<GlTexFormat> gl_tex_format(i32 internal_format, bool legacy_counts) noexcept {
    if (internal_format >= 1 && internal_format <= 4 && !legacy_counts) return std::nullopt;
    for (const FormatRow& row : kFormats) {
        if (static_cast<i64>(row.internal_format) != internal_format) continue;
        GlTexFormat f;
        f.internal_format = row.internal_format;
        f.base = row.base;
        f.r = row.r;
        f.g = row.g;
        f.b = row.b;
        f.a = row.a;
        f.l = row.l;
        f.i = row.i;
        const u8 widest = std::max({row.r, row.g, row.b, row.a, row.l, row.i});
        f.bytes_per_component = widest > 8 ? 2 : 1;
        f.backend = widest > 8 ? graphics::PixelFormat::RGBA16Unorm : graphics::PixelFormat::RGBA8Unorm;
        f.swizzle = swizzle_of(row.base);
        return f;
    }
    return std::nullopt;
}

GlRgba gl_tex_canonical(const GlTexFormat& format, const GlRgba& rgba) noexcept {
    GlRgba out{};
    for (usize k = 0; k < 4; ++k) {
        const u8 from = format.swizzle[k];
        out[k] = from == kZero ? 0.0f : from == kOne ? 1.0f : std::clamp(rgba[from], 0.0f, 1.0f);
    }
    return out;
}

GlRgba gl_tex_return_values(u32 base, const GlRgba& c) noexcept {
    switch (base) {
    case GL_ALPHA: return {0.0f, 0.0f, 0.0f, c[3]};
    case GL_LUMINANCE: return {c[0], 0.0f, 0.0f, 1.0f};
    case GL_LUMINANCE_ALPHA: return {c[0], 0.0f, 0.0f, c[3]};
    case GL_INTENSITY: return {c[0], 0.0f, 0.0f, 1.0f};
    case GL_RGB: return {c[0], c[1], c[2], 1.0f};
    default: return c;
    }
}

bool gl_min_filter_uses_mipmap(u32 min_filter) noexcept {
    return min_filter != GL_NEAREST && min_filter != GL_LINEAR;
}

namespace {

u32 log2_exact(i32 v) noexcept {
    u32 n = 0;
    while ((1 << (n + 1)) <= v) ++n;
    return n;
}

}  // namespace

bool gl_texture_complete(const GlTextureObject& t) noexcept {
    const GlTexLevel& l0 = t.levels[0];
    if (l0.width <= 0 || l0.height <= 0) return false;
    const u32 n = log2_exact(l0.width), m = log2_exact(l0.height);
    const u32 p = std::max(n, m);
    for (u32 d = 1; d <= p; ++d) {
        const GlTexLevel& l = t.levels[d];
        const i32 w = std::max(1, l0.width >> d);
        const i32 h = t.dimensions == 1 ? 1 : std::max(1, l0.height >> d);
        if (l.width != w || l.height != h || l.border != l0.border || l.internal_format != l0.internal_format ||
            l.texels.empty()) {
            return false;
        }
    }
    return true;
}

graphics::TextureStageState gl_tex_env_stage(u32 env_mode, u32 base) noexcept {
    using graphics::TexStageOp;
    constexpr u32 kCurrent = 1, kTexture = 2, kFactor = 3;   // D3DTA_*
    graphics::TextureStageState st{};
    // arg1 = the texture value (Ct, Lt, It or At), arg2 = the fragment
    // colour Cf, arg0 = the texture as Lerp's weight, TFACTOR = Cc.
    st.color_arg0 = kTexture;
    st.color_arg1 = kTexture;
    st.color_arg2 = kCurrent;
    st.alpha_arg0 = kTexture;
    st.alpha_arg1 = kTexture;
    st.alpha_arg2 = kCurrent;
    st.result_arg = kCurrent;
    st.texcoord_index = 0;
    // GL always applies the texture matrix (2.10.2): COUNT4 packs it.
    st.texture_transform_flags = 4;
    const bool has_color = base != GL_ALPHA;
    const bool has_alpha = base == GL_ALPHA || base == GL_LUMINANCE_ALPHA || base == GL_INTENSITY || base == GL_RGBA;
    switch (env_mode) {
    case GL_REPLACE:
        // Table 3.18: Cv = Ct (Lt, It) where the format has colour, else Cf;
        // Av = At (It) where it has alpha, else Af.
        st.color_op = has_color ? TexStageOp::SelectArg1 : TexStageOp::SelectArg2;
        st.alpha_op = has_alpha ? TexStageOp::SelectArg1 : TexStageOp::SelectArg2;
        break;
    case GL_DECAL:
        // Table 3.19: RGB Cv = Ct; RGBA Cv = Cf (1 - At) + Ct At; Av = Af.
        // ALPHA, LUMINANCE, LUMINANCE_ALPHA and INTENSITY are undefined;
        // Vitreo passes the fragment through unchanged (Cv = Cf, Av = Af).
        st.color_op = base == GL_RGB    ? TexStageOp::SelectArg1
                      : base == GL_RGBA ? TexStageOp::BlendTextureAlpha
                                        : TexStageOp::SelectArg2;
        st.alpha_op = TexStageOp::SelectArg2;
        break;
    case GL_BLEND:
        // Table 3.19: Cv = Cf (1 - Ct) + Cc Ct (Lt, It) where the format has
        // colour, else Cf: Lerp(arg0 = texture, arg1 = Cc, arg2 = Cf). Av =
        // Af At (LUMINANCE_ALPHA, RGBA, ALPHA), Af (1 - It) + Ac It
        // (INTENSITY), Af (LUMINANCE, RGB).
        st.color_arg1 = kFactor;
        st.color_op = has_color ? TexStageOp::Lerp : TexStageOp::SelectArg2;
        if (base == GL_INTENSITY) {
            st.alpha_arg1 = kFactor;
            st.alpha_op = TexStageOp::Lerp;
        } else {
            st.alpha_op = has_alpha ? TexStageOp::Modulate : TexStageOp::SelectArg2;
        }
        break;
    case GL_MODULATE:
    default:
        // Table 3.18: Cv = Cf Ct (Lt, It), else Cf; Av = Af At (It), else Af.
        st.color_op = has_color ? TexStageOp::Modulate : TexStageOp::SelectArg2;
        st.alpha_op = has_alpha ? TexStageOp::Modulate : TexStageOp::SelectArg2;
        break;
    }
    return st;
}

namespace {

/// The largest level-of-detail number (log2 of MAX_TEXTURE_SIZE).
constexpr i32 kMaxLevel = static_cast<i32>(kMaxTextureLevels) - 1;

bool is_power_of_two(i32 v) noexcept { return v > 0 && (v & (v - 1)) == 0; }

/// Spec 3.8.1: a dimension is 2^n + 2 bs (or 0, the null texture).
bool dimension_valid(i32 size, i32 border) noexcept {
    if (size == 0) return true;
    return is_power_of_two(size - 2 * border);
}

/// Spec 3.8.1: whether a level-@p level image of (@p w, @p h) can be stored:
/// a complete mipmap down from it must fit MAX_TEXTURE_SIZE at level 0.
bool fits(i32 level, i32 w, i32 h, i32 border) noexcept {
    const i32 limit = static_cast<i32>(kMaxTextureSize) >> level;
    return w - 2 * border <= limit && h - 2 * border <= limit;
}

/// Table 3.5 formats a texture image accepts (3.8.1: not STENCIL_INDEX or
/// DEPTH_COMPONENT).
bool texture_format_valid(u32 format) noexcept {
    switch (format) {
    case GL_COLOR_INDEX: case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_RGB: case GL_RGBA:
    case GL_LUMINANCE: case GL_LUMINANCE_ALPHA:
    case GL_BGR_EXT: case GL_BGRA_EXT:   // GL_EXT_bgra (ext-bgra-teximage2d-*, -texsubimage2d-*)
        return true;
    default: return false;
    }
}

/// Validates format / type; records INVALID_ENUM and returns false.
bool check_format_type(GlContext& ctx, u32 format, u32 type) {
    if (!texture_format_valid(format) || !gl_pixel_type_valid(type, true) ||
        (type == GL_BITMAP && format != GL_COLOR_INDEX)) {
        ctx.record_error(GL_INVALID_ENUM);
        return false;
    }
    return true;
}

/// The object a TEXTURE_1D / TEXTURE_2D target names (@p dimensions 1 / 2).
GlTextureObject& target_object(GlContext& ctx, u32 dimensions, const GlShareGroup::Lock& held) {
    return *ctx.bound_texture(dimensions, held);
}

/// Quantizes a canonical texel into @p out (4 components of @p bpc bytes).
void store_texel(u8* out, const GlRgba& c, u8 bpc) noexcept {
    for (usize k = 0; k < 4; ++k) {
        const double v = std::clamp(static_cast<double>(c[k]), 0.0, 1.0);
        if (bpc == 2) {
            const u16 q = static_cast<u16>(std::floor(v * 65535.0 + 0.5));
            std::memcpy(out + k * 2, &q, 2);
        } else {
            out[k] = static_cast<u8>(std::floor(v * 255.0 + 0.5));
        }
    }
}

GlRgba load_texel(const u8* in, u8 bpc) noexcept {
    GlRgba c{};
    for (usize k = 0; k < 4; ++k) {
        if (bpc == 2) {
            u16 q;
            std::memcpy(&q, in + k * 2, 2);
            c[k] = static_cast<float>(q / 65535.0);
        } else {
            c[k] = static_cast<float>(in[k] / 255.0);
        }
    }
    return c;
}

/// TEXTURE_*_SIZE of a level of @p f: the stored resolution of each
/// component the base format has (spec 6.1.3: "the actual resolutions").
void set_component_sizes(GlTexLevel& l, const GlTexFormat& f) noexcept {
    const u8 bits = static_cast<u8>(f.bytes_per_component * 8);
    l.red_size = l.green_size = l.blue_size = l.alpha_size = l.luminance_size = l.intensity_size = 0;
    switch (f.base) {
    case GL_ALPHA: l.alpha_size = bits; break;
    case GL_LUMINANCE: l.luminance_size = bits; break;
    case GL_LUMINANCE_ALPHA: l.luminance_size = l.alpha_size = bits; break;
    case GL_INTENSITY: l.intensity_size = bits; break;
    case GL_RGB: l.red_size = l.green_size = l.blue_size = bits; break;
    default: l.red_size = l.green_size = l.blue_size = l.alpha_size = bits; break;
    }
}

/// Zero-sized proxy state (3.8.3: "too large": width, height, border and
/// the component resolutions are zero; no error).
void clear_proxy_level(GlTexLevel& l, u32 internal_format) noexcept {
    l = GlTexLevel{};
    l.internal_format = internal_format;
}

void define_level(GlTextureObject& t, i32 level, const GlTexFormat& f, i32 width, i32 height, i32 border) {
    GlTexLevel& l = t.levels[static_cast<usize>(level)];
    l.width = width;
    l.height = height;
    l.border = border;
    l.internal_format = f.internal_format;
    set_component_sizes(l, f);
    l.bytes_per_component = f.bytes_per_component;
    l.indices.clear();
    l.index_mask = 0;
    l.index_base = 0;
    l.texels.assign(static_cast<usize>(width) * static_cast<usize>(height) * 4u * f.bytes_per_component, 0);
    if (width == 0 || height == 0) l.texels.clear();
    t.dirty_levels |= 1u << level;
    t.dirty_rect[static_cast<usize>(level)] = {0, 0, width, height};
}

}  // namespace

/// Marks the whole of @p level changed since the storage was written.
void mark_dirty_level(GlTextureObject& t, i32 level) {
    const GlTexLevel& l = t.levels[static_cast<usize>(level)];
    t.dirty_levels |= 1u << level;
    t.dirty_rect[static_cast<usize>(level)] = {0, 0, l.width, l.height};
}

namespace {

/// Marks texels [x0, x1) x [y0, y1) of @p level changed since the storage
/// was written (the union with what was already marked).
void mark_dirty(GlTextureObject& t, i32 level, i32 x0, i32 y0, i32 x1, i32 y1) {
    std::array<i32, 4>& r = t.dirty_rect[static_cast<usize>(level)];
    if ((t.dirty_levels & (1u << level)) == 0) {
        r = {x0, y0, x1, y1};
    } else {
        r = {std::min(r[0], x0), std::min(r[1], y0), std::max(r[2], x1), std::max(r[3], y1)};
    }
    t.dirty_levels |= 1u << level;
}

/// Writes @p groups (w x h, bottom row first) at (x0, y0) of @p l.
void write_groups(GlTexLevel& l, const GlTexFormat& f, i32 x0, i32 y0, i32 w, i32 h, std::span<const GlRgba> groups) {
    const usize texel_bytes = 4u * l.bytes_per_component;
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            const GlRgba& g = groups[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)];
            const usize at = (static_cast<usize>(y0 + y) * static_cast<usize>(l.width) + static_cast<usize>(x0 + x)) *
                             texel_bytes;
            store_texel(&l.texels[at], gl_tex_canonical(f, g), l.bytes_per_component);
        }
    }
}

std::atomic<bool> g_unreadable_logged{false};

}  // namespace

u16 gl_color_index_mask(i32 internal_format) noexcept {
    switch (internal_format) {
    case GL_COLOR_INDEX1_EXT: case GL_COLOR_INDEX2_EXT: case GL_COLOR_INDEX4_EXT: case GL_COLOR_INDEX8_EXT:
        return 0xFF;
    case GL_COLOR_INDEX12_EXT: case GL_COLOR_INDEX16_EXT: return 0xFFFF;
    default: return 0;
    }
}

void gl_lookup_indices(GlTextureObject& t, i32 level) {
    GlTexLevel& l = t.levels[static_cast<usize>(level)];
    if (l.index_mask == 0 || l.texels.empty()) return;
    const GlColorTable& table = t.color_table;
    l.index_base = table.base;
    const usize n = static_cast<usize>(l.width) * static_cast<usize>(l.height);
    if (l.indices.size() != n || l.texels.size() != n * 4u || table.entries.empty()) return;
    // The width is a power of two: an index past the table wraps (GDI Generic
    // reads past its table there; ext-pal-* does not probe it).
    const usize wrap = table.entries.size() - 1;
    for (usize k = 0; k < n; ++k) {
        const std::array<u8, 4>& e = table.entries[l.indices[k] & wrap];
        l.texels[k * 4 + 0] = e[0];
        l.texels[k * 4 + 1] = e[1];
        l.texels[k * 4 + 2] = e[2];
        l.texels[k * 4 + 3] = table.base == GL_RGB ? u8{255} : e[3];
    }
    mark_dirty_level(t, level);
}

void gl_lookup_all_indices(GlTextureObject& t) {
    for (usize level = 0; level < kMaxTextureLevels; ++level) gl_lookup_indices(t, static_cast<i32>(level));
}

namespace {

/// The level's internal format as a GlTexFormat (TexSubImage assigns by it).
GlTexFormat format_of(const GlTexLevel& l) noexcept {
    if (l.index_mask != 0) {
        // A COLOR_INDEXn_EXT level: its texels are table entries in the
        // canonical layout of the table's base format.
        GlTexFormat f;
        f.internal_format = l.internal_format;
        f.base = l.index_base;
        f.swizzle = swizzle_of(l.index_base);
        return f;
    }
    return gl_tex_format(static_cast<i32>(l.internal_format), true).value_or(GlTexFormat{});
}

/// Stores @p indices (w x h, bottom row first) at (x0, y0) of index level @p l.
void write_indices(GlTexLevel& l, i32 x0, i32 y0, i32 w, i32 h, std::span<const double> indices) {
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            const double v = indices[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)];
            const i64 whole = static_cast<i64>(std::trunc(v));   // ext-pal-teximage-COLOR_INDEX8-float-3.6: 3
            l.indices[static_cast<usize>(y0 + y) * static_cast<usize>(l.width) + static_cast<usize>(x0 + x)] =
                static_cast<u16>(static_cast<u64>(whole) & l.index_mask);
        }
    }
}

/// Spec 3.8.2 (CopyTexImage): the framebuffer rectangle (x, y, w, h) of the
/// read buffer as RGBA groups after the pixel transfer operations, bottom row
/// first. Pixels outside the window are undefined; they read as 0.
void read_framebuffer(GlContext& ctx, i32 x, i32 y, i32 w, i32 h, std::vector<GlRgba>& out) {
    out.assign(static_cast<usize>(w) * static_cast<usize>(h), GlRgba{0.0f, 0.0f, 0.0f, 0.0f});
    std::vector<u8> bgra;
    GlDrawable* d = ctx.drawable.get();
    if (d == nullptr || !d->read_color(ctx.state.pixel_mode.read_buffer, bgra)) return;
    const i32 dw = static_cast<i32>(d->width()), dh = static_cast<i32>(d->height());
    for (i32 j = 0; j < h; ++j) {
        for (i32 i = 0; i < w; ++i) {
            const i32 wx = x + i, wy = y + j;
            if (wx < 0 || wy < 0 || wx >= dw || wy >= dh) continue;
            // The drawable's rows are top-down; window y is bottom-up.
            const u8* p = &bgra[(static_cast<usize>(dh - 1 - wy) * static_cast<usize>(dw) + static_cast<usize>(wx)) * 4u];
            GlRgba c{gl_ubyte_to_float(p[2]), gl_ubyte_to_float(p[1]), gl_ubyte_to_float(p[0]), gl_ubyte_to_float(p[3])};
            gl_transfer_rgba(ctx, c);
            out[static_cast<usize>(j) * static_cast<usize>(w) + static_cast<usize>(i)] = c;
        }
    }
}

/// Parses a TexImage/TexSubImage target. Returns 0 (INVALID_ENUM recorded)
/// for a bad target, else the dimensionality; @p proxy when a proxy.
u32 parse_target(GlContext& ctx, u32 dimensions, u32 target, bool allow_proxy, bool& proxy) {
    proxy = false;
    const u32 real = dimensions == 1 ? GL_TEXTURE_1D : GL_TEXTURE_2D;
    const u32 prox = dimensions == 1 ? GL_PROXY_TEXTURE_1D : GL_PROXY_TEXTURE_2D;
    if (target == real) return dimensions;
    if (allow_proxy && target == prox) {
        proxy = true;
        return dimensions;
    }
    ctx.record_error(GL_INVALID_ENUM);
    return 0;
}

void refuse_border(const char* api) {
    gl_log_unimplemented_once(api, "texture borders (border = 1) are not implemented (Phase 2)");
}

/// TexSubImage / CopyTexSubImage region checks (3.8.2). The region is
/// checked against the level's size first, so a level never specified (size
/// 0) fails any non-empty region with INVALID_VALUE
/// (err-glTexSubImage2D-undefined-level, texsubimage1d-undefined-level,
/// copytexsubimage{1,2}d-undefined-level); an empty region of such a level is
/// INVALID_OPERATION (texsubimage2d-undefined-level-empty).
bool check_sub_region(GlContext& ctx, const GlTexLevel& l, u32 dimensions, i32 xoffset, i32 yoffset, i32 w,
                      i32 h) {
    const i32 b = l.border;
    if (w < 0 || xoffset < -b || xoffset + w > l.width - b) {
        ctx.record_error(GL_INVALID_VALUE);
        return false;
    }
    if (dimensions == 2 && (h < 0 || yoffset < -b || yoffset + h > l.height - b)) {
        ctx.record_error(GL_INVALID_VALUE);
        return false;
    }
    if (l.width <= 0 || l.texels.empty()) {   // the level was never specified
        ctx.record_error(GL_INVALID_OPERATION);
        return false;
    }
    return true;
}

}  // namespace

// ---- Commands ----------------------------------------------------------------------

namespace cmd {

// ---- Texture environment (spec 3.8.5) -----------------------------------------

void TexEnv(GlContext& ctx, u32 target, u32 pname, const GlValues& values) {
    if (ctx.reject_inside_begin_end()) return;
    if (target != GL_TEXTURE_ENV) { ctx.record_error(GL_INVALID_ENUM); return; }
    switch (pname) {
    case GL_TEXTURE_ENV_MODE: {
        const auto mode = as_enum(values[0]);
        if (!mode || (*mode != GL_MODULATE && *mode != GL_DECAL && *mode != GL_BLEND && *mode != GL_REPLACE)) {
            ctx.record_error(GL_INVALID_ENUM);
            return;
        }
        ctx.state.texture.env_mode = *mode;
        break;
    }
    case GL_TEXTURE_ENV_COLOR:
        if (values.scalar) { ctx.record_error(GL_INVALID_ENUM); return; }
        for (u32 i = 0; i < 4; ++i) ctx.state.texture.env_color[i] = clamp01(color_component(values, i));
        break;
    default: ctx.record_error(GL_INVALID_ENUM); break;   // err-bad-enum-glTexEnvi
    }
}

bool GetTexEnv(GlContext& ctx, u32 target, u32 pname, GlQuery& out) {
    if (ctx.reject_inside_begin_end()) return false;
    if (target != GL_TEXTURE_ENV) { ctx.record_error(GL_INVALID_ENUM); return false; }
    const GlTextureState& t = ctx.state.texture;
    switch (pname) {
    case GL_TEXTURE_ENV_MODE: out.set(GlQueryKind::Integer, {static_cast<double>(t.env_mode)}); return true;
    case GL_TEXTURE_ENV_COLOR:
        out.set(GlQueryKind::Color, {t.env_color[0], t.env_color[1], t.env_color[2], t.env_color[3]});
        return true;
    default: ctx.record_error(GL_INVALID_ENUM); return false;
    }
}

// ---- Texture coordinate generation (spec 2.10.4) -------------------------------

void TexGen(GlContext& ctx, u32 coord, u32 pname, const GlValues& values) {
    if (ctx.reject_inside_begin_end()) return;
    if (coord < GL_S || coord > GL_Q) { ctx.record_error(GL_INVALID_ENUM); return; }
    GlTexGen& g = ctx.state.texture.gen[coord - GL_S];
    switch (pname) {
    case GL_TEXTURE_GEN_MODE: {
        const auto mode = as_enum(values[0]);
        const bool ok = mode && (*mode == GL_OBJECT_LINEAR || *mode == GL_EYE_LINEAR ||
                                 (*mode == GL_SPHERE_MAP && coord != GL_R && coord != GL_Q));
        if (!ok) { ctx.record_error(GL_INVALID_ENUM); return; }   // SPHERE_MAP for R/Q: spec 2.10.4
        g.mode = *mode;
        break;
    }
    case GL_OBJECT_PLANE:
        if (values.scalar) { ctx.record_error(GL_INVALID_ENUM); return; }
        g.object_plane = {values[0], values[1], values[2], values[3]};
        break;
    case GL_EYE_PLANE:
        if (values.scalar) { ctx.record_error(GL_INVALID_ENUM); return; }
        // Spec 2.10.4: (p1 p2 p3 p4) M^-1 with M the model-view matrix now.
        g.eye_plane = detail::plane_to_eye(ctx.modelview(), {values[0], values[1], values[2], values[3]});
        break;
    default: ctx.record_error(GL_INVALID_ENUM); break;
    }
}

bool GetTexGen(GlContext& ctx, u32 coord, u32 pname, GlQuery& out) {
    if (ctx.reject_inside_begin_end()) return false;
    if (coord < GL_S || coord > GL_Q) { ctx.record_error(GL_INVALID_ENUM); return false; }
    const GlTexGen& g = ctx.state.texture.gen[coord - GL_S];
    switch (pname) {
    case GL_TEXTURE_GEN_MODE: out.set(GlQueryKind::Integer, {static_cast<double>(g.mode)}); return true;
    case GL_OBJECT_PLANE:
        out.set(GlQueryKind::Float, {g.object_plane[0], g.object_plane[1], g.object_plane[2], g.object_plane[3]});
        return true;
    case GL_EYE_PLANE:
        out.set(GlQueryKind::Float, {g.eye_plane[0], g.eye_plane[1], g.eye_plane[2], g.eye_plane[3]});
        return true;
    default: ctx.record_error(GL_INVALID_ENUM); return false;
    }
}

// ---- Texture object parameters (spec 3.8.4, Table 3.16; 6.1.3) ------------------

namespace {

/// TEXTURE_1D/2D -> the bound object (the default object while 0 is bound,
/// including after the bound object was deleted, 3.8.8); PROXY_TEXTURE_1D/2D
/// -> the proxy (queries only). Null for any other target.
GlTextureObject* texture_for(GlContext& ctx, u32 target, bool allow_proxy, const GlShareGroup::Lock& held) {
    switch (target) {
    case GL_TEXTURE_1D: return ctx.bound_texture(1, held);
    case GL_TEXTURE_2D: return ctx.bound_texture(2, held);
    case GL_PROXY_TEXTURE_1D: return allow_proxy ? &ctx.proxy_textures[0] : nullptr;
    case GL_PROXY_TEXTURE_2D: return allow_proxy ? &ctx.proxy_textures[1] : nullptr;
    default: return nullptr;
    }
}

}  // namespace

void TexParameter(GlContext& ctx, u32 target, u32 pname, const GlValues& values) {
    if (ctx.reject_inside_begin_end()) return;
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlTextureObject* t = texture_for(ctx, target, false, held);
    if (!t) { ctx.record_error(GL_INVALID_ENUM); return; }   // err-bad-enum-glTexParameteri
    GlTextureParams& p = t->params;
    const auto e = as_enum(values[0]);
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (!e || (*e != GL_NEAREST && *e != GL_LINEAR && *e != GL_NEAREST_MIPMAP_NEAREST &&
                   *e != GL_LINEAR_MIPMAP_NEAREST && *e != GL_NEAREST_MIPMAP_LINEAR &&
                   *e != GL_LINEAR_MIPMAP_LINEAR)) {
            ctx.record_error(GL_INVALID_ENUM);
            return;
        }
        p.min_filter = *e;
        break;
    case GL_TEXTURE_MAG_FILTER:
        if (!e || (*e != GL_NEAREST && *e != GL_LINEAR)) { ctx.record_error(GL_INVALID_ENUM); return; }
        p.mag_filter = *e;
        break;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        // GL 1.1 wrap modes are CLAMP and REPEAT only (CLAMP_TO_EDGE is 1.2).
        if (!e || (*e != GL_CLAMP && *e != GL_REPEAT)) { ctx.record_error(GL_INVALID_ENUM); return; }
        (pname == GL_TEXTURE_WRAP_S ? p.wrap_s : p.wrap_t) = *e;
        break;
    case GL_TEXTURE_BORDER_COLOR:
        if (values.scalar) { ctx.record_error(GL_INVALID_ENUM); return; }
        // Spec 3.8: Table 2.6 signed conversion for integers, clamped to [0, 1].
        for (u32 i = 0; i < 4; ++i) p.border_color[i] = clamp01(color_component(values, i));
        break;
    case GL_TEXTURE_PRIORITY:
        p.priority = clamp01(values[0]);   // spec 3.8.4: clamped to [0, 1]
        break;
    default: ctx.record_error(GL_INVALID_ENUM); break;   // TEXTURE_RESIDENT is read-only
    }
}

bool GetTexParameter(GlContext& ctx, u32 target, u32 pname, GlQuery& out) {
    if (ctx.reject_inside_begin_end()) return false;
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlTextureObject* t = texture_for(ctx, target, true, held);
    if (!t) { ctx.record_error(GL_INVALID_ENUM); return false; }
    const GlTextureParams& p = t->params;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: out.set(GlQueryKind::Integer, {static_cast<double>(p.min_filter)}); break;
    case GL_TEXTURE_MAG_FILTER: out.set(GlQueryKind::Integer, {static_cast<double>(p.mag_filter)}); break;
    case GL_TEXTURE_WRAP_S: out.set(GlQueryKind::Integer, {static_cast<double>(p.wrap_s)}); break;
    case GL_TEXTURE_WRAP_T: out.set(GlQueryKind::Integer, {static_cast<double>(p.wrap_t)}); break;
    case GL_TEXTURE_BORDER_COLOR:
        out.set(GlQueryKind::Color, {p.border_color[0], p.border_color[1], p.border_color[2], p.border_color[3]});
        break;
    case GL_TEXTURE_PRIORITY: out.set(GlQueryKind::Float, {p.priority}); break;
    case GL_TEXTURE_RESIDENT: out.set(GlQueryKind::Boolean, {t->resident ? 1.0 : 0.0}); break;
    default: ctx.record_error(GL_INVALID_ENUM); return false;
    }
    return true;
}

bool GetTexLevelParameter(GlContext& ctx, u32 target, i32 level, u32 pname, GlQuery& out) {
    if (ctx.reject_inside_begin_end()) return false;
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlTextureObject* t = texture_for(ctx, target, true, held);
    if (!t) { ctx.record_error(GL_INVALID_ENUM); return false; }
    // Spec 6.1.3: lod < 0 or above the maximum level -> INVALID_VALUE.
    if (level < 0 || static_cast<u32>(level) >= kMaxTextureLevels) {
        ctx.record_error(GL_INVALID_VALUE);
        return false;
    }
    const GlTexLevel& l = t->levels[static_cast<usize>(level)];
    double v = 0;
    switch (pname) {
    case GL_TEXTURE_WIDTH: v = l.width; break;
    case GL_TEXTURE_HEIGHT: v = l.height; break;
    case GL_TEXTURE_BORDER: v = l.border; break;
    case GL_TEXTURE_INTERNAL_FORMAT: v = l.internal_format; break;   // == TEXTURE_COMPONENTS
    case GL_TEXTURE_RED_SIZE: v = l.red_size; break;
    case GL_TEXTURE_GREEN_SIZE: v = l.green_size; break;
    case GL_TEXTURE_BLUE_SIZE: v = l.blue_size; break;
    case GL_TEXTURE_ALPHA_SIZE: v = l.alpha_size; break;
    case GL_TEXTURE_LUMINANCE_SIZE: v = l.luminance_size; break;
    case GL_TEXTURE_INTENSITY_SIZE: v = l.intensity_size; break;
    default: ctx.record_error(GL_INVALID_ENUM); return false;
    }
    out.set(GlQueryKind::Integer, {v});
    return true;
}

// ---- Texture images (spec 3.8.1-3.8.3) --------------------------------------------

void TexImage(GlContext& ctx, u32 dimensions, u32 target, i32 level, i32 internal_format, i32 width,
              i32 height, i32 border, u32 format, u32 type, const GlClientPixels& pixels) {
    if (ctx.reject_inside_begin_end()) return;
    bool proxy = false;
    if (parse_target(ctx, dimensions, target, true, proxy) == 0) return;
    if (dimensions == 1) height = 1;   // 3.8.1: a 1D image's height is always 1
    if (level < 0 || level > kMaxLevel) { ctx.record_error(GL_INVALID_VALUE); return; }
    // GDI Generic records INVALID_ENUM for an internal format that is not one
    // of Tables 3.14-3.15 or 1-4 (teximage-bad-internalformat-{1d,2d}-0x1234,
    // -0, -5, -GL_RED, -GL_COLOR_INDEX; err-bad-components-glTexImage2D-5),
    // and checks it before the size (teximage-bad-internalformat-and-npot).
    // The 1.1 text says INVALID_VALUE; the measured behaviour is what
    // applications were tested against.
    const u16 index_mask = gl_color_index_mask(internal_format);
    std::optional<GlTexFormat> f = gl_tex_format(internal_format, true);
    if (!f && index_mask != 0) {
        // GL_EXT_paletted_texture: a COLOR_INDEXn_EXT image (ext-pal-teximage-*).
        f = GlTexFormat{};
        f->internal_format = static_cast<u32>(internal_format);
        f->base = GL_RGBA;
    }
    if (!f) { ctx.record_error(GL_INVALID_ENUM); return; }
    if (border < 0 || border > 1 || width < 0 || height < 0 || !dimension_valid(width, border) ||
        (dimensions == 2 && !dimension_valid(height, border))) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    if (!check_format_type(ctx, format, type)) return;
    // An index texture takes index data only (ext-pal-teximage-COLOR_INDEX8-from-rgba).
    if (index_mask != 0 && format != GL_COLOR_INDEX) { ctx.record_error(GL_INVALID_OPERATION); return; }
    const char* api = dimensions == 1 ? "glTexImage1D" : "glTexImage2D";
    const bool storable = border == 0 && fits(level, width, dimensions == 1 ? 1 : height, border);
    if (proxy) {
        // 3.8.3: no image is transferred; the proxy level is what the image
        // would be, or zero-sized when it could not be stored.
        GlTexLevel& l = ctx.proxy_textures[dimensions - 1].levels[static_cast<usize>(level)];
        if (border != 0) refuse_border(api);
        if (!storable) {
            clear_proxy_level(l, f->internal_format);
            return;
        }
        l = GlTexLevel{};
        l.width = width;
        l.height = height;
        l.border = border;
        l.internal_format = f->internal_format;
        if (index_mask == 0) set_component_sizes(l, *f);   // an index level reports 0 (ext-pal-teximage-*)
        return;
    }
    if (border != 0) {
        // Design 11: texture borders are Phase 2.
        refuse_border(api);
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    if (!storable) { ctx.record_error(GL_INVALID_VALUE); return; }   // too large under any conditions

    std::vector<GlRgba> groups;
    std::vector<double> indices;
    if (!pixels.null_pointer && width > 0 && height > 0) {
        if (!pixels.readable) {
            gl_log_once(g_unreadable_logged, "a texture image's client memory is unreadable; the command has no effect");
            return;
        }
        const usize count = static_cast<usize>(width) * static_cast<usize>(height);
        // A short result means the host could not hold the groups.
        if (index_mask != 0) {
            gl_unpack_color_indices(ctx, gl_unpack_modes(ctx, pixels), type, width, height, pixels.bytes, indices);
            if (indices.size() != count) { ctx.record_error(GL_OUT_OF_MEMORY); return; }
        } else {
            gl_unpack_rgba(ctx, gl_unpack_modes(ctx, pixels), format, type, width, height, pixels.bytes, groups);
            if (groups.size() != count) { ctx.record_error(GL_OUT_OF_MEMORY); return; }
        }
    }
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlTextureObject& t = target_object(ctx, dimensions, held);
    define_level(t, level, *f, width, height, border);
    GlTexLevel& l = t.levels[static_cast<usize>(level)];
    if (index_mask != 0) {
        l.red_size = l.green_size = l.blue_size = l.alpha_size = l.luminance_size = l.intensity_size = 0;
        l.index_mask = index_mask;
        l.indices.assign(static_cast<usize>(width) * static_cast<usize>(height), 0);
        if (!indices.empty()) write_indices(l, 0, 0, width, height, indices);
        gl_lookup_indices(t, level);
    }
    if (!groups.empty()) write_groups(l, *f, 0, 0, width, height, groups);
    // Resident once level 0 holds client data; a NULL-pointer image is not
    // (texparam-resident-after-teximage 1, -after-null-teximage 0).
    if (level == 0) t.resident = !groups.empty() || !indices.empty();
}

void TexSubImage(GlContext& ctx, u32 dimensions, u32 target, i32 level, i32 xoffset, i32 yoffset, i32 width,
                 i32 height, u32 format, u32 type, const GlClientPixels& pixels) {
    if (ctx.reject_inside_begin_end()) return;
    bool proxy = false;
    if (parse_target(ctx, dimensions, target, false, proxy) == 0) return;
    if (dimensions == 1) {
        yoffset = 0;
        height = 1;
    }
    if (level < 0 || level > kMaxLevel) { ctx.record_error(GL_INVALID_VALUE); return; }
    if (!check_format_type(ctx, format, type)) return;
    if (width < 0 || height < 0) { ctx.record_error(GL_INVALID_VALUE); return; }
    std::vector<GlRgba> groups;
    {
        const GlShareGroup::Lock held = ctx.share_group->lock();
        GlTextureObject& t = target_object(ctx, dimensions, held);
        if (!check_sub_region(ctx, t.levels[static_cast<usize>(level)], dimensions, xoffset, yoffset, width,
                              height)) {
            return;
        }
    }
    bool index_level = false;
    {
        const GlShareGroup::Lock held = ctx.share_group->lock();
        index_level = target_object(ctx, dimensions, held).levels[static_cast<usize>(level)].index_mask != 0;
    }
    // GL_EXT_paletted_texture: an index level takes index data only
    // (ext-pal-texsubimage-rgba-into-index INVALID_ENUM, -texsubimage-index).
    if (index_level && format != GL_COLOR_INDEX) { ctx.record_error(GL_INVALID_ENUM); return; }
    if (width == 0 || height == 0) return;
    if (pixels.null_pointer || !pixels.readable) {
        gl_log_once(g_unreadable_logged, "a texture image's client memory is unreadable; the command has no effect");
        return;
    }
    const usize count = static_cast<usize>(width) * static_cast<usize>(height);
    std::vector<double> indices;
    if (index_level) {
        gl_unpack_color_indices(ctx, gl_unpack_modes(ctx, pixels), type, width, height, pixels.bytes, indices);
        if (indices.size() != count) { ctx.record_error(GL_OUT_OF_MEMORY); return; }
    } else {
        gl_unpack_rgba(ctx, gl_unpack_modes(ctx, pixels), format, type, width, height, pixels.bytes, groups);
        if (groups.size() != count) { ctx.record_error(GL_OUT_OF_MEMORY); return; }
    }
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlTextureObject& t = target_object(ctx, dimensions, held);
    GlTexLevel& l = t.levels[static_cast<usize>(level)];
    // Another context may have redefined the level meanwhile: re-check.
    if (l.texels.empty() || xoffset + width > l.width || yoffset + height > l.height) return;
    if (index_level) {
        if (l.index_mask == 0 || l.indices.size() != static_cast<usize>(l.width) * static_cast<usize>(l.height)) return;
        write_indices(l, xoffset, yoffset, width, height, indices);
        gl_lookup_indices(t, level);
        return;
    }
    write_groups(l, format_of(l), xoffset, yoffset, width, height, groups);
    mark_dirty(t, level, xoffset, yoffset, xoffset + width, yoffset + height);
}

void CopyTexImage(GlContext& ctx, u32 dimensions, u32 target, i32 level, u32 internal_format, i32 x, i32 y,
                  i32 width, i32 height, i32 border) {
    if (ctx.reject_inside_begin_end()) return;
    bool proxy = false;
    if (parse_target(ctx, dimensions, target, false, proxy) == 0) return;
    if (dimensions == 1) height = 1;
    if (level < 0 || level > kMaxLevel) { ctx.record_error(GL_INVALID_VALUE); return; }
    // 3.8.2: internalformat may not be 1-4; an invalid one is INVALID_ENUM
    // (GDI Generic: copyteximage-bad-internalformat-{1d,2d}-1..4, -0x1234,
    // -0, -5, -GL_RED, -GL_COLOR_INDEX; checked before the size,
    // copyteximage-bad-internalformat-and-npot).
    // GL_EXT_paletted_texture: the framebuffer holds no indices
    // (ext-pal-copyteximage-index8: INVALID_OPERATION).
    if (internal_format <= 0x7FFFFFFFu && gl_color_index_mask(static_cast<i32>(internal_format)) != 0) {
        ctx.record_error(GL_INVALID_OPERATION);
        return;
    }
    const std::optional<GlTexFormat> f =
        internal_format > 0x7FFFFFFFu ? std::nullopt : gl_tex_format(static_cast<i32>(internal_format), false);
    if (!f) { ctx.record_error(GL_INVALID_ENUM); return; }
    if (border < 0 || border > 1 || width < 0 || height < 0 || !dimension_valid(width, border) ||
        (dimensions == 2 && !dimension_valid(height, border))) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    if (border != 0) {
        refuse_border(dimensions == 1 ? "glCopyTexImage1D" : "glCopyTexImage2D");
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    if (!fits(level, width, height, border)) { ctx.record_error(GL_INVALID_VALUE); return; }
    // The framebuffer is read before the share group is locked (lock order,
    // GlShareGroup); reading ends the drawable's pass.
    std::vector<GlRgba> groups;
    if (width > 0 && height > 0) read_framebuffer(ctx, x, y, width, height, groups);
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlTextureObject& t = target_object(ctx, dimensions, held);
    define_level(t, level, *f, width, height, border);
    if (!groups.empty()) write_groups(t.levels[static_cast<usize>(level)], *f, 0, 0, width, height, groups);
    // Level 0 now holds framebuffer data (residency as for glTexImage with
    // data; not measured for copies).
    if (level == 0) t.resident = !groups.empty();
}

void CopyTexSubImage(GlContext& ctx, u32 dimensions, u32 target, i32 level, i32 xoffset, i32 yoffset, i32 x,
                     i32 y, i32 width, i32 height) {
    if (ctx.reject_inside_begin_end()) return;
    bool proxy = false;
    if (parse_target(ctx, dimensions, target, false, proxy) == 0) return;
    if (dimensions == 1) {
        yoffset = 0;
        height = 1;
    }
    if (level < 0 || level > kMaxLevel) { ctx.record_error(GL_INVALID_VALUE); return; }
    if (width < 0 || height < 0) { ctx.record_error(GL_INVALID_VALUE); return; }
    {
        const GlShareGroup::Lock held = ctx.share_group->lock();
        GlTextureObject& t = target_object(ctx, dimensions, held);
        if (!check_sub_region(ctx, t.levels[static_cast<usize>(level)], dimensions, xoffset, yoffset, width,
                              height)) {
            return;
        }
    }
    if (width == 0 || height == 0) return;
    std::vector<GlRgba> groups;
    read_framebuffer(ctx, x, y, width, height, groups);
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlTextureObject& t = target_object(ctx, dimensions, held);
    GlTexLevel& l = t.levels[static_cast<usize>(level)];
    if (l.texels.empty() || xoffset + width > l.width || yoffset + height > l.height) return;
    write_groups(l, format_of(l), xoffset, yoffset, width, height, groups);
    mark_dirty(t, level, xoffset, yoffset, xoffset + width, yoffset + height);
}

bool GetTexImage(GlContext& ctx, u32 target, i32 level, u32 format, u32 type, GlClientBuffer client, void* user) {
    if (ctx.reject_inside_begin_end()) return false;
    if (target != GL_TEXTURE_1D && target != GL_TEXTURE_2D) { ctx.record_error(GL_INVALID_ENUM); return false; }
    if (level < 0 || level > kMaxLevel) { ctx.record_error(GL_INVALID_VALUE); return false; }
    // 6.1.4: COLOR_INDEX, STENCIL_INDEX and DEPTH_COMPONENT are INVALID_ENUM;
    // GL_EXT_paletted_texture returns an index level's indices for
    // COLOR_INDEX (ext-pal-teximage-*-get-index).
    if (!texture_format_valid(format) || !gl_pixel_type_valid(type, false)) {
        ctx.record_error(GL_INVALID_ENUM);
        return false;
    }
    std::vector<GlRgba> groups;
    std::vector<double> indices;
    i32 w = 0, h = 0;
    {
        const GlShareGroup::Lock held = ctx.share_group->lock();
        const GlTextureObject& t = target_object(ctx, target == GL_TEXTURE_1D ? 1 : 2, held);
        const GlTexLevel& l = t.levels[static_cast<usize>(level)];
        if (format == GL_COLOR_INDEX && l.index_mask == 0) {
            ctx.record_error(GL_INVALID_ENUM);
            return false;
        }
        if (l.texels.empty()) return false;
        w = l.width;
        h = l.height;
        if (format == GL_COLOR_INDEX) indices.assign(l.indices.begin(), l.indices.end());
        const u32 base = format_of(l).base;
        groups.reserve(static_cast<usize>(w) * static_cast<usize>(h));
        const usize texel_bytes = 4u * l.bytes_per_component;
        for (usize k = 0; k < static_cast<usize>(w) * static_cast<usize>(h); ++k) {
            groups.push_back(gl_tex_return_values(base, load_texel(&l.texels[k * texel_bytes], l.bytes_per_component)));
        }
    }
    const GlPixelStoreModes& pack = ctx.state.pixel_store.pack;
    const GlPixelLayout layout = gl_pixel_layout(pack, format, type, w, h);
    const std::span<u8> out = layout.valid ? client(user, layout.span_bytes) : std::span<u8>{};
    if (!layout.valid || out.size() < layout.span_bytes) {
        gl_log_once(g_unreadable_logged, "a texture image's client memory is unreadable; the command has no effect");
        return false;
    }
    // GL 1.1 6.1.4: the groups are "packed and placed in client memory as
    // described in section 4.3.2 under ReadPixels" -- packing and placement;
    // 6.1.4 names no pixel transfer operation for them.
    if (format == GL_COLOR_INDEX) gl_pack_indices(pack, type, w, h, indices, out);
    else gl_pack_rgba(pack, format, type, w, h, groups, out);
    return true;
}

// ---- Texture objects (spec 3.8.8) ----------------------------------------------------

void GenTextures(GlContext& ctx, i32 n, std::vector<u32>& names) {
    names.clear();
    if (ctx.reject_inside_begin_end()) return;
    if (n < 0) { ctx.record_error(GL_INVALID_VALUE); return; }
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlShareGroup::TextureMap& map = ctx.share_group->textures(held);
    u32& next = ctx.share_group->next_texture_name(held);
    for (i32 k = 0; k < n; ++k) {
        // Previously unused names, marked used (a null object until bound).
        while (next == 0 || map.count(next) != 0) ++next;
        map.emplace(next, nullptr);
        names.push_back(next++);
    }
}

void DeleteTextures(GlContext& ctx, i32 n, std::span<const u32> names) {
    if (ctx.reject_inside_begin_end()) return;
    if (n < 0) { ctx.record_error(GL_INVALID_VALUE); return; }
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlShareGroup::TextureMap& map = ctx.share_group->textures(held);
    for (const u32 name : names) {
        if (name == 0) continue;   // unused names and zero are silently ignored
        const auto it = map.find(name);
        if (it == map.end()) continue;
        // Deleting a bound texture binds 0 to its target in this context.
        if (ctx.state.texture.binding_1d == name) {
            ctx.state.texture.binding_1d = 0;
            ctx.bound_objects[0].reset();
        }
        if (ctx.state.texture.binding_2d == name) {
            ctx.state.texture.binding_2d = 0;
            ctx.bound_objects[1].reset();
        }
        map.erase(it);
    }
}

void BindTexture(GlContext& ctx, u32 target, u32 name) {
    if (ctx.reject_inside_begin_end()) return;
    if (target != GL_TEXTURE_1D && target != GL_TEXTURE_2D) { ctx.record_error(GL_INVALID_ENUM); return; }
    const u32 dimensions = target == GL_TEXTURE_1D ? 1 : 2;
    const usize idx = dimensions - 1;
    u32& binding = dimensions == 1 ? ctx.state.texture.binding_1d : ctx.state.texture.binding_2d;
    const GlShareGroup::Lock held = ctx.share_group->lock();
    if (name == 0) {
        binding = 0;
        ctx.bound_objects[idx].reset();
        return;
    }
    GlShareGroup::TextureMap& map = ctx.share_group->textures(held);
    std::shared_ptr<GlTextureObject>& slot = map[name];
    if (slot && slot->dimensions != 0 && slot->dimensions != dimensions) {
        ctx.record_error(GL_INVALID_OPERATION);   // a 1D object to TEXTURE_2D or the reverse
        return;
    }
    if (!slot) {
        // Binding an unused (or generated) name creates the object with the
        // initial state of 3.8.3 and fixes its dimensionality.
        slot = std::make_shared<GlTextureObject>(gl_initial_texture_object(name));
        slot->dimensions = dimensions;
    }
    binding = name;
    ctx.bound_objects[idx] = slot;
}

bool IsTexture(GlContext& ctx, u32 name) {
    if (ctx.reject_inside_begin_end()) return false;
    if (name == 0) return false;
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlShareGroup::TextureMap& map = ctx.share_group->textures(held);
    const auto it = map.find(name);
    // A generated name that was never bound is not a texture object.
    return it != map.end() && it->second && it->second->dimensions != 0;
}

GlResidency AreTexturesResident(GlContext& ctx, i32 n, std::span<const u32> names, std::vector<u8>& residences) {
    residences.clear();
    if (ctx.reject_inside_begin_end()) return {};
    if (n < 0) { ctx.record_error(GL_INVALID_VALUE); return {}; }
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlShareGroup::TextureMap& map = ctx.share_group->textures(held);
    // GDI Generic writes each name's residence in order, also when all are
    // resident, and stops at the first name that is not a texture object
    // with INVALID_VALUE, leaving the residences before it written
    // (aretexturesresident-after-teximage res=1,1; -zero / -unknown /
    // -generated-unbound res=1,untouched).
    bool all = true;
    for (const u32 name : names) {
        const auto it = name == 0 ? map.end() : map.find(name);
        if (it == map.end() || !it->second || it->second->dimensions == 0) {
            ctx.record_error(GL_INVALID_VALUE);   // not the name of a texture object
            return {false, true};
        }
        residences.push_back(it->second->resident ? 1 : 0);
        all = all && it->second->resident;
    }
    return {all, true};
}

void PrioritizeTextures(GlContext& ctx, i32 n, std::span<const u32> names, std::span<const float> priorities) {
    if (ctx.reject_inside_begin_end()) return;
    if (n < 0) { ctx.record_error(GL_INVALID_VALUE); return; }
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlShareGroup::TextureMap& map = ctx.share_group->textures(held);
    for (usize k = 0; k < names.size() && k < priorities.size(); ++k) {
        // Unused names and the default textures (0) are silently ignored.
        if (names[k] == 0) continue;
        const auto it = map.find(names[k]);
        if (it == map.end() || !it->second || it->second->dimensions == 0) continue;
        it->second->params.priority = clamp01(priorities[k]);
    }
}

}  // namespace cmd

// ---- The texture a draw samples (spec 3.8.6, 3.8.2) ------------------------------------

namespace {

std::atomic<bool> g_storage_refused{false};

std::atomic<u64> g_storages_created{0}, g_renames{0}, g_budget_flushes{0}, g_bytes_uploaded{0};

usize storage_bytes(const GlTextureObject& t, u32 level_count) {
    usize bytes = 0;
    for (u32 d = 0; d < level_count; ++d) bytes += t.levels[d].texels.size();
    return bytes;
}

/// Uploads the dirty rectangle of each dirty level < @p level_count (all of
/// it when @p whole).
bool upload_levels(GlTextureObject& t, u32 level_count, bool whole) {
    graphics::BackendTexture2D& tex = *t.storage->texture;
    for (u32 d = 0; d < level_count; ++d) {
        if (!whole && (t.dirty_levels & (1u << d)) == 0) continue;
        const GlTexLevel& l = t.levels[d];
        const usize texel = 4u * l.bytes_per_component;
        const unsigned pitch = static_cast<unsigned>(l.width * static_cast<i32>(texel));
        std::array<i32, 4> r = whole ? std::array<i32, 4>{0, 0, l.width, l.height} : t.dirty_rect[d];
        r = {std::clamp(r[0], 0, l.width), std::clamp(r[1], 0, l.height), std::clamp(r[2], 0, l.width),
             std::clamp(r[3], 0, l.height)};
        if (r[2] <= r[0] || r[3] <= r[1]) continue;
        const u8* first = l.texels.data() + (static_cast<usize>(r[1]) * l.width + r[0]) * texel;
        const bool ok = (r[0] == 0 && r[1] == 0 && r[2] == l.width && r[3] == l.height)
                            ? tex.update(d, l.texels.data(), pitch)
                            : tex.update_region(d, 0, first, pitch, static_cast<unsigned>(r[0]),
                                                static_cast<unsigned>(r[1]), static_cast<unsigned>(r[2] - r[0]),
                                                static_cast<unsigned>(r[3] - r[1]));
        if (!ok) return false;
        g_bytes_uploaded += static_cast<u64>(r[2] - r[0]) * static_cast<u64>(r[3] - r[1]) * texel;
    }
    return true;
}

std::shared_ptr<GlTextureStorage> create_storage(const GlTextureObject& t, u32 level_count,
                                                 graphics::GraphicsBackendDevice& device) {
    const GlTexLevel& l0 = t.levels[0];
    graphics::BackendTexture2DDesc desc{};
    desc.width = static_cast<unsigned>(l0.width);
    desc.height = static_cast<unsigned>(l0.height);
    desc.mip_levels = level_count;
    desc.pixel_format = format_of(l0).backend;
    desc.shader_resource = true;
    std::unique_ptr<graphics::BackendTexture2D> created = device.create_texture_2d(desc, nullptr, 0);
    if (!created || !created->valid()) return nullptr;
    ++g_storages_created;
    auto storage = std::make_shared<GlTextureStorage>();
    storage->texture = std::move(created);
    storage->bytes = storage_bytes(t, level_count);
    return storage;
}

/// The backend storage of @p t holding @p level_count levels, refreshed from
/// its image arrays (under @p held, the share group's lock):
///   - shape changed (size, level count, format) or no storage: new storage,
///     every level uploaded;
///   - nothing changed: as is;
///   - changed, storage not captured by a draw since it was written: the
///     dirty rectangles are uploaded in place;
///   - changed, storage captured (a pending draw or open pass may still
///     read it, spec 2.5): renamed -- new storage, the old one's levels
///     copied into it on the GPU (in order with the recorded work), then
///     only the dirty rectangles uploaded. The old storage lives until the
///     draws that captured it have executed.
/// Cost bound. Each rename keeps one more copy alive until its passes end,
/// so renames are budgeted per share group (kGlTextureRenameBudget bytes of
/// renamed storage). A rename past the budget instead ends the open pass of
/// every drawable that captured the storage (recording their pending draws
/// first), which orders the in-place upload after them, and resets the
/// budget. Memory held by renames is therefore at most the budget plus one
/// texture per share group, and an n-texel change uploads n texels.
bool refresh_storage(GlContext& ctx, GlTextureObject& t, u32 level_count, graphics::GraphicsBackendDevice& device,
                     const GlShareGroup::Lock& held) {
    const GlTexLevel& l0 = t.levels[0];
    const GlTexFormat f = format_of(l0);
    const auto matches = [&](const graphics::BackendTexture2D& tex) {
        const graphics::BackendTexture2DDesc& d = tex.desc();
        return d.width == static_cast<unsigned>(l0.width) && d.height == static_cast<unsigned>(l0.height) &&
               d.mip_levels == level_count && d.pixel_format == f.backend;
    };
    const u32 all_levels = level_count >= 32 ? ~0u : (1u << level_count) - 1u;
    const bool reuse = t.storage && t.storage->texture && matches(*t.storage->texture);
    const auto fail = [&]() {
        gl_log_once(g_storage_refused, "the backend refused a texture's storage; primitives draw untextured");
        t.storage.reset();
        t.storage_in_use = false;
        return false;
    };
    if (!reuse) {
        t.storage = create_storage(t, level_count, device);
        if (!t.storage) return fail();
        t.storage_in_use = false;
        if (!upload_levels(t, level_count, true)) return fail();
        t.dirty_levels = 0;
        return true;
    }
    if ((t.dirty_levels & all_levels) == 0) return true;
    if (t.storage_in_use) {
        usize& renamed = ctx.share_group->renamed_bytes(held);
        if (renamed + t.storage->bytes <= kGlTextureRenameBudget) {
            std::shared_ptr<GlTextureStorage> next = create_storage(t, level_count, device);
            if (!next) return fail();
            bool copied = true;
            for (u32 d = 0; d < level_count && copied; ++d) {
                copied = device.copy_texture_2d_region(next->texture.get(), 0, 0, t.storage->texture.get(), 0, 0,
                                                       static_cast<unsigned>(t.levels[d].width),
                                                       static_cast<unsigned>(t.levels[d].height), d, 0, d, 0);
            }
            renamed += next->bytes;
            ++g_renames;
            t.storage = std::move(next);
            t.storage_in_use = false;
            if (!upload_levels(t, level_count, !copied)) return fail();
            t.dirty_levels = 0;
            return true;
        }
        // Over budget: end the passes that may read the storage, then write
        // it in place (lock order: share group -> drawable, GlShareGroup).
        for (const std::weak_ptr<GlDrawable>& user : t.storage->users) {
            if (const std::shared_ptr<GlDrawable> d = user.lock()) d->end_pass();
        }
        t.storage->users.clear();
        t.storage_in_use = false;
        renamed = 0;
        ++g_budget_flushes;
    }
    if (!upload_levels(t, level_count, false)) return fail();
    t.dirty_levels = 0;
    return true;
}

}  // namespace

GlTextureStats gl_texture_stats() noexcept {
    return {g_storages_created.load(), g_renames.load(), g_budget_flushes.load(), g_bytes_uploaded.load()};
}

GlDrawTexture gl_texture_for_draw(GlContext& ctx) {
    GlDrawTexture out;
    const GlState& s = ctx.state;
    // 3.8.6: two-dimensional texturing wins over one-dimensional.
    const u32 dimensions = s.enabled(GlCap::Texture2D) ? 2 : s.enabled(GlCap::Texture1D) ? 1 : 0;
    if (dimensions == 0) return out;
    graphics::GraphicsBackendDevice* device = graphics::active_backend_device();
    if (device == nullptr) return out;
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlTextureObject& t = *ctx.bound_texture(dimensions, held);
    const GlTexLevel& l0 = t.levels[0];
    // The null texture at level 0 (3.8.1), or a mipmap filter on an
    // incomplete set of arrays (3.8.2): as if texturing were disabled.
    if (l0.width <= 0 || l0.height <= 0 || l0.texels.empty()) return out;
    const bool complete = gl_texture_complete(t);
    const bool mipmapped = gl_min_filter_uses_mipmap(t.params.min_filter);
    if (mipmapped && !complete) return out;
    const u32 n = log2_exact(l0.width), m = dimensions == 1 ? 0 : log2_exact(l0.height);
    const u32 p = std::max(n, m);
    // A complete mipmap goes to the backend whole, so a filter change needs
    // no rebuild; otherwise level 0 alone.
    if (!refresh_storage(ctx, t, complete ? p + 1 : 1, *device, held)) return out;
    t.storage_in_use = true;
    if (ctx.drawable) {
        // This draw's drawable may now read the storage until its pass ends.
        std::vector<std::weak_ptr<GlDrawable>>& users = t.storage->users;
        std::erase_if(users, [](const std::weak_ptr<GlDrawable>& u) { return u.expired(); });
        const bool known = std::any_of(users.begin(), users.end(), [&](const std::weak_ptr<GlDrawable>& u) {
            return u.lock() == ctx.drawable;
        });
        if (!known) users.push_back(ctx.drawable);
    }
    const GlTexFormat f = format_of(l0);
    out.enabled = true;
    out.storage = t.storage;
    out.stage = gl_tex_env_stage(s.texture.env_mode, f.base);
    graphics::FFUniformInputs::GlTextureUnit& u = out.unit;
    u.min_filter = t.params.min_filter;
    u.mag_filter = t.params.mag_filter;
    u.wrap_s = t.params.wrap_s;
    u.wrap_t = t.params.wrap_t;
    u.log2_width = n;
    u.log2_height = m;
    u.top_level = complete ? p : 0;
    u.dimensions = dimensions;
    // 3.8.1: the border colour is interpreted to match the internal format
    // (Table 3.15), i.e. stored the way a texel of that format is.
    u.border = gl_tex_canonical(f, {t.params.border_color[0], t.params.border_color[1], t.params.border_color[2],
                                    t.params.border_color[3]});
    return out;
}

}  // namespace vitreo::winapi::gl
