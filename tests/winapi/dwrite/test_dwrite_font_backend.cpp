// SPDX-License-Identifier: LGPL-2.1-only
#include <gtest/gtest.h>

#include "winapi/graphics/dwrite/dwrite.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

TEST(DWriteFontBackendTest, SystemFallbackRetainsReopenableNativeIdentity) {
    const auto fallback = dwrite::map_system_font(
        u"Spotify", {}, 400, 5, 0, u"en-US");
    ASSERT_TRUE(fallback.has_value());
    ASSERT_TRUE(fallback->has_font);
    EXPECT_TRUE(fallback->font.system_ui_font);
    EXPECT_FALSE(fallback->font.family_name.empty());

    const auto glyphs = dwrite::glyph_indices(
        fallback->font,
        {static_cast<u32>(u'S'), static_cast<u32>(u'p'),
         static_cast<u32>(u'o')});
    ASSERT_TRUE(glyphs.has_value());
    ASSERT_EQ(glyphs->size(), 3u);
    EXPECT_NE((*glyphs)[0], 0u);
    EXPECT_NE((*glyphs)[1], 0u);
    EXPECT_NE((*glyphs)[2], 0u);
}

TEST(DWriteFontBackendTest, CopiesOpenTypeTablesUsingDirectWriteTagOrder) {
    const auto font = dwrite::match_system_font(u"Arial", 400, 5, 0);
    ASSERT_TRUE(font.has_value());

    // DWRITE_MAKE_OPENTYPE_TAG('c', 'm', 'a', 'p').
    const auto cmap = dwrite::font_table(*font, 0x70616d63u);
    ASSERT_TRUE(cmap.has_value());
    EXPECT_FALSE(cmap->empty());

    const auto missing = dwrite::font_table(*font, 0x7a7a7a7au);
    ASSERT_TRUE(missing.has_value());
    EXPECT_TRUE(missing->empty());
}

TEST(DWriteFontBackendTest, ParsesPublishedSystemFontBytes) {
    const auto font = dwrite::match_system_font(u"Arial", 400, 5, 0);
    ASSERT_TRUE(font.has_value());
    ASSERT_FALSE(font->source_path.empty());
    const auto data = dwrite::font_file_data(*font);
    ASSERT_TRUE(data.has_value());
    ASSERT_FALSE(data->empty());
    const auto parsed = dwrite::fonts_from_data(*data);
    ASSERT_FALSE(parsed.empty());
    EXPECT_TRUE(std::any_of(parsed.begin(), parsed.end(), [](const auto& face) {
        return !face.family_name.empty() && !face.postscript_name.empty();
    }));
}

TEST(DWriteFontBackendTest, ParsedFaceRetainsItsNativeIdentityAndBytes) {
    const auto system_font =
        dwrite::match_system_font(u"Arial", 400, 5, 0);
    ASSERT_TRUE(system_font.has_value());
    const auto data = dwrite::font_file_data(*system_font);
    ASSERT_TRUE(data.has_value());
    auto parsed = dwrite::fonts_from_data(*data);
    ASSERT_FALSE(parsed.empty());

    auto retained = parsed.front();
    ASSERT_TRUE(retained.native_face);
    ASSERT_TRUE(retained.source_data);
    EXPECT_EQ(*retained.source_data, *data);
    const auto glyphs = dwrite::glyph_indices(
        retained, {static_cast<u32>(u'M')});
    ASSERT_TRUE(glyphs.has_value());
    ASSERT_EQ(glyphs->size(), 1u);
    ASSERT_NE(glyphs->front(), 0u);
    const auto expected = dwrite::glyph_metrics(retained, *glyphs);
    ASSERT_TRUE(expected.has_value());
    ASSERT_EQ(expected->size(), 1u);

    // A custom loader's PostScript name may not be registered with CoreText.
    // Reopening must use the retained parsed face rather than name matching.
    retained.family_name = u"Vitreo missing custom family";
    retained.postscript_name = u"VitreoMissingCustomPostScriptName";
    const auto actual = dwrite::glyph_metrics(retained, *glyphs);
    ASSERT_TRUE(actual.has_value());
    ASSERT_EQ(actual->size(), 1u);
    EXPECT_EQ(actual->front().advance_width,
              expected->front().advance_width);
    EXPECT_EQ(actual->front().left_side_bearing,
              expected->front().left_side_bearing);
    EXPECT_EQ(actual->front().right_side_bearing,
              expected->front().right_side_bearing);
}

namespace {

u16 be16(const std::vector<u8>& bytes, usize offset) {
    return static_cast<u16>(
        (static_cast<u16>(bytes[offset]) << 8) | bytes[offset + 1]);
}

u32 be32(const std::vector<u8>& bytes, usize offset) {
    return (static_cast<u32>(bytes[offset]) << 24) |
           (static_cast<u32>(bytes[offset + 1]) << 16) |
           (static_cast<u32>(bytes[offset + 2]) << 8) |
           bytes[offset + 3];
}

// Nominal cmap GID (platform 3/encoding 1 or 10, format 4/12), or 0xFFFF if
// the table cannot be walked. Matches IDWriteFontFace::GetGlyphIndices.
u16 cmap_glyph(const std::vector<u8>& cmap, u32 codepoint) {
    if (cmap.size() < 4) return 0xFFFF;
    const u16 num_tables = be16(cmap, 2);
    usize best_offset = 0;
    u16 best_format = 0;
    for (u16 i = 0; i < num_tables; ++i) {
        const usize rec = 4 + static_cast<usize>(i) * 8;
        if (rec + 8 > cmap.size()) break;
        const u16 platform = be16(cmap, rec);
        const u16 encoding = be16(cmap, rec + 2);
        const u32 offset = be32(cmap, rec + 4);
        if (offset + 2 > cmap.size() || platform != 3) continue;
        const u16 format = be16(cmap, offset);
        if (encoding == 10 && format == 12) {
            best_offset = offset;
            best_format = 12;
            break;
        }
        if (encoding == 1 && format == 4 && best_format != 12) {
            best_offset = offset;
            best_format = 4;
        }
    }
    if (best_format == 4) {
        if (codepoint > 0xFFFF || best_offset + 14 > cmap.size()) return 0;
        const u16 seg_count_x2 = be16(cmap, best_offset + 6);
        const u16 seg_count = seg_count_x2 / 2;
        const usize end_code = best_offset + 14;
        const usize start_code = end_code + seg_count_x2 + 2;
        const usize id_delta = start_code + seg_count_x2;
        const usize id_range = id_delta + seg_count_x2;
        if (id_range + seg_count_x2 > cmap.size()) return 0;
        for (u16 s = 0; s < seg_count; ++s) {
            const u16 end = be16(cmap, end_code + s * 2);
            const u16 start = be16(cmap, start_code + s * 2);
            if (codepoint < start || codepoint > end) continue;
            const u16 range = be16(cmap, id_range + s * 2);
            const i16 delta = static_cast<i16>(be16(cmap, id_delta + s * 2));
            if (range == 0) {
                return static_cast<u16>(static_cast<u32>(codepoint) + delta);
            }
            const usize glyph_off =
                id_range + s * 2 + range + (codepoint - start) * 2;
            if (glyph_off + 2 > cmap.size()) return 0;
            const u16 glyph = be16(cmap, glyph_off);
            if (glyph == 0) return 0;
            return static_cast<u16>(glyph + delta);
        }
        return 0;
    }
    if (best_format == 12) {
        if (best_offset + 16 > cmap.size()) return 0;
        const u32 n_groups = be32(cmap, best_offset + 12);
        usize group = best_offset + 16;
        for (u32 g = 0; g < n_groups; ++g, group += 12) {
            if (group + 12 > cmap.size()) break;
            const u32 start = be32(cmap, group);
            const u32 end = be32(cmap, group + 4);
            const u32 start_glyph = be32(cmap, group + 8);
            if (codepoint < start || codepoint > end) continue;
            return static_cast<u16>(start_glyph + (codepoint - start));
        }
        return 0;
    }
    return 0xFFFF;
}

} // namespace

TEST(DWriteFontBackendTest, GetGlyphIndicesMatchCmapAndEmitLatinOutlines) {
    const auto font = dwrite::match_system_font(u"Arial", 400, 5, 0);
    ASSERT_TRUE(font.has_value());
    const auto cmap = dwrite::font_table(*font, 0x70616d63u);
    ASSERT_TRUE(cmap.has_value());
    ASSERT_FALSE(cmap->empty());

    for (u32 ch = u'a'; ch <= u'z'; ++ch) {
        const auto glyphs = dwrite::glyph_indices(*font, {ch});
        ASSERT_TRUE(glyphs.has_value()) << ch;
        ASSERT_EQ(glyphs->size(), 1u);
        const u16 cmap_gid = cmap_glyph(*cmap, ch);
        EXPECT_EQ(glyphs->front(), cmap_gid) << "codepoint " << ch;
        ASSERT_NE(glyphs->front(), 0u) << ch;

        dwrite::GlyphOutlineParams params{};
        params.font = *font;
        params.em_size = 16.0f;
        params.glyph_indices = *glyphs;
        const auto outline = dwrite::glyph_run_outline(params);
        ASSERT_TRUE(outline.has_value()) << static_cast<char>(ch);
        EXPECT_FALSE(outline->figures.empty()) << static_cast<char>(ch);
        for (const auto& figure : outline->figures) {
            EXPECT_TRUE(figure.closed) << static_cast<char>(ch);
        }
    }
}

TEST(DWriteFontBackendTest, LatinGlyphsRasterizeWithCoverageAtBodySize) {
    const auto count_coverage = [](const dwrite::RasterizedGlyphRun& raster) {
        u32 covered = 0;
        for (u8 alpha : raster.alpha) {
            if (alpha != 0) ++covered;
        }
        return covered;
    };
    const auto rasterize_letter = [](const dwrite::MatchedFont& font, u16 glyph,
                                     bool antialias) {
        dwrite::GlyphRunRasterParams params{};
        params.font = font;
        params.font_em_size = 16.0f;
        params.pixels_per_dip = 1.0f;
        params.glyph_indices = {glyph};
        return dwrite::rasterize_glyph_run(params, antialias);
    };
    const auto check_font = [&](const dwrite::MatchedFont& font,
                                const char* label) {
        const auto cmap = dwrite::font_table(font, 0x70616d63u);
        ASSERT_TRUE(cmap.has_value()) << label;
        ASSERT_FALSE(cmap->empty()) << label;
        for (u32 ch = u'a'; ch <= u'z'; ++ch) {
            const auto glyphs = dwrite::glyph_indices(font, {ch});
            ASSERT_TRUE(glyphs.has_value()) << label << ' ' << static_cast<char>(ch);
            ASSERT_EQ(glyphs->size(), 1u);
            const u16 cg_glyph = glyphs->front();
            const u16 cmap_gid = cmap_glyph(*cmap, ch);
            ASSERT_NE(cg_glyph, 0u) << label << ' ' << static_cast<char>(ch);
            ASSERT_NE(cmap_gid, 0u) << label << ' ' << static_cast<char>(ch);

            const auto from_indices = rasterize_letter(font, cg_glyph, true);
            ASSERT_TRUE(from_indices.has_value())
                << label << ' ' << static_cast<char>(ch);
            EXPECT_GT(count_coverage(*from_indices), 4u)
                << label << " GetGlyphIndices '" << static_cast<char>(ch)
                << "' gid=" << cg_glyph
                << " bounds=(" << from_indices->left << ',' << from_indices->top
                << ")-(" << from_indices->right << ',' << from_indices->bottom
                << ')';

            const auto from_cmap = rasterize_letter(font, cmap_gid, true);
            ASSERT_TRUE(from_cmap.has_value())
                << label << ' ' << static_cast<char>(ch);
            EXPECT_GT(count_coverage(*from_cmap), 4u)
                << label << " cmap '" << static_cast<char>(ch)
                << "' gid=" << cmap_gid
                << " bounds=(" << from_cmap->left << ',' << from_cmap->top
                << ")-(" << from_cmap->right << ',' << from_cmap->bottom
                << ')';

            const auto aliased = rasterize_letter(font, cg_glyph, false);
            ASSERT_TRUE(aliased.has_value())
                << label << " aliased " << static_cast<char>(ch);
            EXPECT_GT(count_coverage(*aliased), 4u)
                << label << " aliased '" << static_cast<char>(ch)
                << "' gid=" << cg_glyph;
        }

        const auto h_glyphs = dwrite::glyph_indices(font, {u'h'});
        ASSERT_TRUE(h_glyphs.has_value());
        const auto h_aa = rasterize_letter(font, h_glyphs->front(), true);
        ASSERT_TRUE(h_aa.has_value());
        u8 max_alpha = 0;
        u32 sum = 0;
        for (u8 alpha : h_aa->alpha) {
            max_alpha = std::max(max_alpha, alpha);
            sum += alpha;
        }
        EXPECT_GE(max_alpha, 200)
            << label << " 'h' max alpha " << static_cast<int>(max_alpha)
            << " mean "
            << (h_aa->alpha.empty() ? 0 : sum / h_aa->alpha.size())
            << " coverage " << count_coverage(*h_aa)
            << " bounds=(" << h_aa->left << ',' << h_aa->top << ")-("
            << h_aa->right << ',' << h_aa->bottom << ')';
    };

    const auto arial = dwrite::match_system_font(u"Arial", 400, 5, 0);
    ASSERT_TRUE(arial.has_value());
    check_font(*arial, "Arial");

    const auto ui = dwrite::match_system_ui_font(400, 5, 0);
    ASSERT_TRUE(ui.has_value());
    check_font(*ui, "system-ui");
}

TEST(DWriteFontBackendTest, GlyphRunOutlineEmitsFiguresForLatinGlyph) {
    const auto font = dwrite::match_system_font(u"Arial", 400, 5, 0);
    ASSERT_TRUE(font.has_value());
    const auto glyphs = dwrite::glyph_indices(
        *font, {static_cast<u32>(u'A')});
    ASSERT_TRUE(glyphs.has_value());
    ASSERT_EQ(glyphs->size(), 1u);
    ASSERT_NE(glyphs->front(), 0u);

    dwrite::GlyphOutlineParams params{};
    params.font = *font;
    params.em_size = 24.0f;
    params.glyph_indices = *glyphs;
    const auto outline = dwrite::glyph_run_outline(params);
    ASSERT_TRUE(outline.has_value());
    EXPECT_EQ(outline->fill_mode, 1u);
    ASSERT_FALSE(outline->figures.empty());
    EXPECT_FALSE(outline->figures.front().segments.empty());

    // Sink space is +Y-down: a Latin capital's outline must extend above the
    // baseline (negative Y), not below it.
    float min_y = outline->figures.front().start.y;
    float max_y = min_y;
    for (const auto& figure : outline->figures) {
        min_y = std::min(min_y, figure.start.y);
        max_y = std::max(max_y, figure.start.y);
        for (const auto& segment : figure.segments) {
            if (segment.kind == dwrite::OutlineSegmentKind::Line) {
                min_y = std::min(min_y, segment.line.y);
                max_y = std::max(max_y, segment.line.y);
            } else {
                min_y = std::min(min_y, segment.bezier.end.y);
                max_y = std::max(max_y, segment.bezier.end.y);
            }
        }
    }
    EXPECT_LT(min_y, 0.0f);
    EXPECT_LT(std::abs(max_y), std::abs(min_y));
}
