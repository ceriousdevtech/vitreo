// SPDX-License-Identifier: LGPL-2.1-only

/// @file font_data_glyph_access.cpp
/// @brief Font data and glyph-space access for the DC's selected font.
///
/// GetFontData, GetGlyphIndices and GetGlyphOutline are the exports a font
/// engine uses when it does its own shaping and rasterization instead of
/// handing strings to TextOut. A guest that reads the 'cmap' table itself maps
/// every character through GetFontData; failing that call leaves it with glyph
/// index 0 for every character, which draws nothing at all rather than drawing
/// wrong. So these three exports resolve the DC's logical font to a real face
/// and answer from that face's own tables and outlines.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace vitreo::winapi {

// resolve_dc_font / dc_font_em_pixels live in text_engine.cpp: the face GDI
// maps the logical font to and its pixels per em.

namespace gdi_font_data {

u32 get_font_data(GdiManager& mgr, Marshaler& m) {
    constexpr u32 GDI_ERROR = 0xFFFFFFFFu;
    const auto hdc = static_cast<Handle>(m.arg(0));
    const u32 table = static_cast<u32>(m.arg(1));
    const u32 offset = static_cast<u32>(m.arg(2));
    const GuestAddr buffer = m.arg_ptr(3);
    const u32 buffer_size = static_cast<u32>(m.arg(4));
    if (hdc == 0) return GDI_ERROR;

    auto font = mgr.resolve_dc_font(hdc);
    if (!font) return GDI_ERROR;

    // A table tag of zero asks for the whole font file.
    std::optional<std::vector<u8>> bytes =
        table == 0 ? dwrite::font_file_data(*font)
                   : dwrite::font_table(*font, table);
    if (!bytes || bytes->empty()) return GDI_ERROR;
    if (offset > bytes->size()) return GDI_ERROR;

    const u32 available = static_cast<u32>(bytes->size() - offset);
    // A null buffer is a size query for the whole table, ignoring the offset.
    if (buffer == 0) return static_cast<u32>(bytes->size());

    const u32 copied = std::min(available, buffer_size);
    for (u32 index = 0; index < copied; ++index) {
        if (m.write_u8(buffer + index, (*bytes)[offset + index]).is_err()) {
            return GDI_ERROR;
        }
    }
    return copied;
}

u32 get_glyph_indices(GdiManager& mgr, Marshaler& m, bool wide) {
    constexpr u32 GDI_ERROR = 0xFFFFFFFFu;
    constexpr u32 GGI_MARK_NONEXISTING_GLYPHS = 0x1;
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr text = m.arg_ptr(1);
    const i32 count = static_cast<i32>(m.arg(2));
    const GuestAddr glyphs_out = m.arg_ptr(3);
    const u32 flags = static_cast<u32>(m.arg(4));
    if (hdc == 0 || text == 0 || glyphs_out == 0 || count < 0) {
        return GDI_ERROR;
    }

    auto font = mgr.resolve_dc_font(hdc);
    if (!font) return GDI_ERROR;

    std::vector<u32> codepoints;
    codepoints.reserve(static_cast<usize>(count));
    for (i32 index = 0; index < count; ++index) {
        if (wide) {
            const auto unit = m.read_u16(text + static_cast<u32>(index) * 2);
            if (unit.is_err()) return GDI_ERROR;
            codepoints.push_back(unit.value());
        } else {
            const auto unit = m.read_u8(text + static_cast<u32>(index));
            if (unit.is_err()) return GDI_ERROR;
            codepoints.push_back(unit.value());
        }
    }

    auto glyphs = dwrite::glyph_indices(*font, codepoints);
    if (!glyphs) return GDI_ERROR;

    // 0xFFFF is what GDI stores for a character the font cannot represent when
    // the caller asks to have those marked.
    constexpr u16 GLYPH_NOT_PRESENT = 0xFFFF;
    for (usize index = 0; index < glyphs->size(); ++index) {
        u16 glyph = (*glyphs)[index];
        if (glyph == 0 && (flags & GGI_MARK_NONEXISTING_GLYPHS) != 0) {
            glyph = GLYPH_NOT_PRESENT;
        }
        if (m.write_u16(glyphs_out + static_cast<u32>(index) * 2, glyph)
                .is_err()) {
            return GDI_ERROR;
        }
    }
    return static_cast<u32>(glyphs->size());
}

u32 get_char_widths_i(GdiManager& mgr, Marshaler& m, bool abc) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const u32 first_glyph = static_cast<u32>(m.arg(1));
    const u32 count = static_cast<u32>(m.arg(2));
    const GuestAddr glyph_array = m.arg_ptr(3);
    const GuestAddr widths_out = m.arg_ptr(4);
    if (hdc == 0 || widths_out == 0 || count == 0 || count > 65536) return 0;

    std::vector<u16> glyphs;
    glyphs.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        if (glyph_array == 0) {
            glyphs.push_back(static_cast<u16>(first_glyph + index));
        } else {
            const auto glyph = m.read_u16(glyph_array + index * 2);
            if (glyph.is_err()) return 0;
            glyphs.push_back(static_cast<u16>(glyph.value()));
        }
    }
    // Device advances, as GetCharWidth32 reports them (s3gdi_otm_probe
    // "GetCharWidthI=1 9,7,3,3,7" for "Hello" in Arial -13).
    std::vector<std::array<i32, 3>> spacing;
    auto advances = mgr.glyph_advances(hdc, glyphs, abc ? &spacing : nullptr);
    if (!advances || advances->size() != glyphs.size()) return 0;
    for (u32 index = 0; index < count; ++index) {
        if (!abc) {
            if (m.write_u32(widths_out + index * 4, static_cast<u32>((*advances)[index])).is_err()) return 0;
            continue;
        }
        const GuestAddr base = widths_out + static_cast<GuestAddr>(index) * 12;
        if (m.write_u32(base + 0, static_cast<u32>(spacing[index][0])).is_err() ||
            m.write_u32(base + 4, static_cast<u32>(spacing[index][1])).is_err() ||
            m.write_u32(base + 8, static_cast<u32>(spacing[index][2])).is_err()) {
            return 0;
        }
    }
    return 1;
}

namespace {

struct GlyphRaster {
    dwrite::RasterizedGlyphRun run;
    i32 advance{0};
};

/// Rasterizes one glyph with the baseline origin at (0,0), so the run bounds
/// are already the glyph's black box relative to that origin.
std::optional<GlyphRaster> rasterize_single_glyph(
    const dwrite::MatchedFont& font, float em_pixels, u16 glyph_index) {
    dwrite::GlyphRunRasterParams params;
    params.font = font;
    params.font_em_size = em_pixels;
    params.glyph_indices.push_back(glyph_index);
    params.glyph_advances.push_back(0.0f);
    params.glyph_offsets.push_back({});

    auto rasterized = dwrite::rasterize_glyph_run(params, true);
    if (!rasterized) return std::nullopt;

    GlyphRaster result;
    result.run = std::move(*rasterized);
    auto metrics = dwrite::glyph_metrics(font, {glyph_index});
    if (metrics && !metrics->empty() &&
        font.metrics.design_units_per_em != 0) {
        const float scale =
            em_pixels / static_cast<float>(font.metrics.design_units_per_em);
        result.advance = static_cast<i32>(std::lround(
            static_cast<float>((*metrics)[0].advance_width) * scale));
    }
    return result;
}

/// GDI packs glyph bitmap rows to a DWORD boundary, and each gray format has
/// its own number of coverage levels rather than a plain 0..255 ramp.
u32 gray_row_stride(u32 width) { return (width + 3u) & ~3u; }

u32 mono_row_stride(u32 width) { return ((width + 31u) / 32u) * 4u; }

u8 quantize_coverage(u8 alpha, u32 levels) {
    // GDI's gray formats count levels, so the top value is levels, not
    // levels - 1: GGO_GRAY8_BITMAP ranges over 0..64.
    return static_cast<u8>((static_cast<u32>(alpha) * levels + 127u) / 255u);
}

}  // namespace

u32 get_glyph_outline(GdiManager& mgr, Marshaler& m) {
    constexpr u32 GDI_ERROR = 0xFFFFFFFFu;
    constexpr u32 GGO_METRICS = 0;
    constexpr u32 GGO_BITMAP = 1;
    constexpr u32 GGO_GRAY2_BITMAP = 4;
    constexpr u32 GGO_GRAY4_BITMAP = 5;
    constexpr u32 GGO_GRAY8_BITMAP = 6;
    constexpr u32 GGO_GLYPH_INDEX = 0x0080;

    const auto hdc = static_cast<Handle>(m.arg(0));
    const u32 character = static_cast<u32>(m.arg(1));
    const u32 raw_format = static_cast<u32>(m.arg(2));
    const GuestAddr metrics_out = m.arg_ptr(3);
    const u32 buffer_size = static_cast<u32>(m.arg(4));
    const GuestAddr buffer = m.arg_ptr(5);
    const GuestAddr transform = m.arg_ptr(6);
    if (hdc == 0 || metrics_out == 0 || transform == 0) return GDI_ERROR;

    const bool by_index = (raw_format & GGO_GLYPH_INDEX) != 0;
    const u32 format = raw_format & ~GGO_GLYPH_INDEX;

    auto font = mgr.resolve_dc_font(hdc);
    if (!font) return GDI_ERROR;
    const float em_pixels = mgr.dc_font_em_pixels(hdc);

    u16 glyph = static_cast<u16>(character);
    if (!by_index) {
        auto mapped = dwrite::glyph_indices(*font, {character});
        if (!mapped || mapped->empty()) return GDI_ERROR;
        glyph = (*mapped)[0];
    }

    auto raster = rasterize_single_glyph(*font, em_pixels, glyph);
    if (!raster) return GDI_ERROR;

    const i32 raster_width = raster->run.right - raster->run.left;
    const i32 raster_height = raster->run.bottom - raster->run.top;
    const u32 black_box_x =
        raster_width > 0 ? static_cast<u32>(raster_width) : 0u;
    const u32 black_box_y =
        raster_height > 0 ? static_cast<u32>(raster_height) : 0u;

    // GLYPHMETRICS measures the origin from the baseline with y growing up,
    // the opposite of the rasterizer's device-space y.
    if (m.write_u32(metrics_out + 0, black_box_x).is_err() ||
        m.write_u32(metrics_out + 4, black_box_y).is_err() ||
        m.write_u32(metrics_out + 8,
                    static_cast<u32>(raster->run.left)).is_err() ||
        m.write_u32(metrics_out + 12,
                    static_cast<u32>(-raster->run.top)).is_err() ||
        m.write_u16(metrics_out + 16,
                    static_cast<u16>(raster->advance)).is_err() ||
        m.write_u16(metrics_out + 18, 0).is_err()) {
        return GDI_ERROR;
    }

    if (format == GGO_METRICS) return 0;

    u32 levels = 0;
    switch (format) {
    case GGO_BITMAP: levels = 1; break;
    case GGO_GRAY2_BITMAP: levels = 4; break;
    case GGO_GRAY4_BITMAP: levels = 16; break;
    case GGO_GRAY8_BITMAP: levels = 64; break;
    default:
        // Outline formats are not served from a rasterized run.
        return GDI_ERROR;
    }

    const u32 stride = format == GGO_BITMAP ? mono_row_stride(black_box_x)
                                            : gray_row_stride(black_box_x);
    const u32 required = stride * black_box_y;
    if (buffer == 0 || buffer_size == 0) return required;
    if (buffer_size < required) return GDI_ERROR;
    if (required == 0) return 0;

    const usize coverage_count =
        static_cast<usize>(black_box_x) * static_cast<usize>(black_box_y);
    if (raster->run.alpha.size() < coverage_count) return GDI_ERROR;

    std::vector<u8> rows(required, 0);
    for (u32 y = 0; y < black_box_y; ++y) {
        for (u32 x = 0; x < black_box_x; ++x) {
            const u8 alpha =
                raster->run.alpha[static_cast<usize>(y) * black_box_x + x];
            if (format == GGO_BITMAP) {
                if (alpha >= 128) {
                    rows[y * stride + x / 8] |=
                        static_cast<u8>(0x80u >> (x % 8));
                }
            } else {
                rows[y * stride + x] = quantize_coverage(alpha, levels);
            }
        }
    }
    for (u32 index = 0; index < required; ++index) {
        if (m.write_u8(buffer + index, rows[index]).is_err()) {
            return GDI_ERROR;
        }
    }
    return required;
}

}  // namespace gdi_font_data

}  // namespace vitreo::winapi
