// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi::dwrite {

struct FontMetrics {
    std::uint16_t design_units_per_em{0};
    std::uint16_t ascent{0};
    std::uint16_t descent{0};
    std::int16_t line_gap{0};
    std::uint16_t cap_height{0};
    std::uint16_t x_height{0};
    std::int16_t underline_position{0};
    std::uint16_t underline_thickness{0};
    std::int16_t strikethrough_position{0};
    std::uint16_t strikethrough_thickness{0};
};

struct GlyphMetrics {
    std::int32_t left_side_bearing{0};
    std::uint32_t advance_width{0};
    std::int32_t right_side_bearing{0};
    std::int32_t top_side_bearing{0};
    std::uint32_t advance_height{0};
    std::int32_t bottom_side_bearing{0};
    std::int32_t vertical_origin_y{0};
};

struct GlyphOffset {
    float advance_offset{0};
    float ascender_offset{0};
};

struct RasterizedGlyphRun {
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};
    std::vector<std::uint8_t> alpha;
};

struct MatchedFont {
    std::uint32_t weight{400};
    std::uint32_t stretch{5};
    std::uint32_t style{0};
    std::uint32_t face_type{6};
    std::uint16_t glyph_count{0};
    bool system_ui_font{false};
    std::u16string family_name;
    std::u16string face_name;
    std::u16string postscript_name;
    // Host implementation detail used to publish a real local-font reference
    // into the capsule. This path is never returned directly to the guest.
    std::string source_path;
    // Custom-loader faces are not necessarily installed on the host. Retain
    // both their parsed native identity and original bytes so reopening the
    // face cannot silently substitute a system font with different glyph IDs
    // or design units.
    std::shared_ptr<void> native_face;
    std::shared_ptr<const std::vector<std::uint8_t>> source_data;
    std::uint32_t face_index{0};
    FontMetrics metrics{};
};

struct GlyphRunRasterParams {
    MatchedFont font{};
    float font_em_size{0};
    float pixels_per_dip{1};
    std::array<float, 6> transform{1, 0, 0, 1, 0, 0};
    float baseline_origin_x{0};
    float baseline_origin_y{0};
    std::vector<std::uint16_t> glyph_indices;
    std::vector<float> glyph_advances;
    std::vector<GlyphOffset> glyph_offsets;
    bool is_sideways{false};
    std::uint32_t bidi_level{0};
};

struct OutlinePoint {
    float x{0};
    float y{0};
};

struct OutlineBezier {
    OutlinePoint control1{};
    OutlinePoint control2{};
    OutlinePoint end{};
};

enum class OutlineSegmentKind {
    Line,
    Bezier,
};

struct OutlineSegment {
    OutlineSegmentKind kind{OutlineSegmentKind::Line};
    OutlinePoint line{};
    OutlineBezier bezier{};
};

struct OutlineFigure {
    OutlinePoint start{};
    bool closed{false};
    std::vector<OutlineSegment> segments;
};

struct GlyphRunOutline {
    // D2D1_FILL_MODE_WINDING — glyph contours use non-zero winding.
    std::uint32_t fill_mode{1};
    std::vector<OutlineFigure> figures;
};

struct GlyphOutlineParams {
    MatchedFont font{};
    float em_size{0};
    std::vector<std::uint16_t> glyph_indices;
    std::vector<float> glyph_advances;
    std::vector<GlyphOffset> glyph_offsets;
    bool is_right_to_left{false};
};

struct FallbackMatch {
    std::uint32_t mapped_length{0};
    bool has_font{false};
    std::u16string family_name;
    MatchedFont font{};
};

[[nodiscard]] std::vector<std::u16string> enumerate_system_font_families();
[[nodiscard]] std::optional<MatchedFont> match_system_font(
    const std::u16string& family,
    std::uint32_t weight,
    std::uint32_t stretch,
    std::uint32_t style);
[[nodiscard]] std::optional<MatchedFont> match_system_ui_font(
    std::uint32_t weight,
    std::uint32_t stretch,
    std::uint32_t style);
[[nodiscard]] std::vector<MatchedFont> enumerate_system_family_fonts(
    const std::u16string& family);
[[nodiscard]] std::optional<FallbackMatch> map_system_font(
    const std::u16string& text,
    const std::u16string& base_family,
    std::uint32_t weight,
    std::uint32_t stretch,
    std::uint32_t style,
    const std::u16string& locale);
[[nodiscard]] std::optional<std::vector<std::uint16_t>> glyph_indices(
    const MatchedFont& font,
    const std::vector<std::uint32_t>& codepoints);
[[nodiscard]] std::optional<std::vector<GlyphMetrics>> glyph_metrics(
    const MatchedFont& font,
    const std::vector<std::uint16_t>& glyph_indices);
[[nodiscard]] std::optional<RasterizedGlyphRun> rasterize_glyph_run(
    const GlyphRunRasterParams& params, bool antialias);
[[nodiscard]] std::optional<GlyphRunOutline> glyph_run_outline(
    const GlyphOutlineParams& params);

/// Copies an OpenType table from the matched native face. The tag uses
/// DWRITE_MAKE_OPENTYPE_TAG byte order (the first character is the low byte).
/// A disengaged result means the native face could not be reopened; an empty
/// vector means the face exists but does not contain the requested table.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> font_table(
    const MatchedFont& font, std::uint32_t open_type_tag);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> font_file_data(
    const MatchedFont& font);
[[nodiscard]] std::vector<MatchedFont> fonts_from_data(
    const std::vector<std::uint8_t>& data);
/// As above, but the parsed faces share @p data instead of a private copy.
[[nodiscard]] std::vector<MatchedFont> fonts_from_data(
    std::shared_ptr<const std::vector<std::uint8_t>> data);

} // namespace vitreo::winapi::dwrite
