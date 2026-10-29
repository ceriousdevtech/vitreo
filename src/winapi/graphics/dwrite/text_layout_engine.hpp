// SPDX-License-Identifier: LGPL-2.1-only
// IDWriteTextLayout's formatting model on the host: itemization, shaping,
// font fallback, bidi, line breaking, alignment, trimming and the metrics
// DirectWrite reports. CoreText's typesetter does shaping and break
// opportunities over the same font files DirectWrite exposes; the line
// model (heights, baselines, alignment) is DirectWrite's, measured by
// tools/oracle_probe/dwrite_text_layout_probe.
#pragma once

#include "winapi/graphics/dwrite/font_backend.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi::dwrite {

/// An inline object's metrics (IDWriteInlineObject::GetMetrics) and break
/// conditions, read from the guest before layout.
struct InlineObjectMetrics {
    float width{0};
    float height{0};
    float baseline{0};
    std::uint32_t break_before{0};
    std::uint32_t break_after{0};
};

/// Formatting of one span of text. Equal styles share one entry.
struct TextLayoutStyle {
    MatchedFont font{};           ///< face resolved from family/weight/style/stretch
    bool font_resolved{false};    ///< false: the family does not exist, fall back
    std::uint32_t simulations{0}; ///< DWRITE_FONT_SIMULATIONS of `font`
    float size{0};
    std::u16string locale;
    bool underline{false};
    bool strikethrough{false};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> features;  ///< tag, value
    bool inline_object{false};
    InlineObjectMetrics inline_metrics{};
};

struct TextLayoutParams {
    std::u16string text;
    std::vector<TextLayoutStyle> styles;
    std::vector<std::uint32_t> style_of;  ///< per UTF-16 unit, index into styles
    std::uint32_t base_style{0};          ///< the format's style (empty text, final empty line)
    float max_width{0};
    float max_height{0};
    std::uint32_t text_alignment{0};
    std::uint32_t paragraph_alignment{0};
    std::uint32_t word_wrapping{0};
    std::uint32_t reading_direction{0};
    std::uint32_t flow_direction{0};
    float incremental_tab_stop{0};
    std::uint32_t line_spacing_method{0};
    float line_spacing{0};
    float baseline{0};
    std::uint32_t trimming_granularity{0};
    std::uint32_t trimming_delimiter{0};
    std::uint32_t trimming_delimiter_count{0};
    bool has_trimming_sign{false};
    InlineObjectMetrics trimming_sign{};
};

struct LayoutGlyphRun {
    std::uint32_t text_start{0};
    std::uint32_t text_length{0};
    std::uint32_t style{0};
    MatchedFont font{};
    std::uint32_t simulations{0};
    float size{0};
    std::uint32_t bidi_level{0};
    std::uint32_t line{0};
    float baseline_x{0};  ///< DIPs from the layout origin; RTL runs start at their right edge
    float baseline_y{0};
    float width{0};
    std::vector<std::uint16_t> glyphs;
    std::vector<float> advances;
    std::vector<GlyphOffset> offsets;
    std::vector<std::uint16_t> cluster_map;  ///< text_length entries
    bool inline_object{false};
    bool trimming_sign{false};
    float inline_baseline{0};  ///< object baseline from its top (inline/sign runs)
};

struct LayoutCluster {
    std::uint32_t text_start{0};
    std::uint32_t length{0};
    float width{0};
    float x{0};  ///< left edge in layout coordinates
    std::uint32_t line{0};
    std::uint32_t bidi_level{0};
    bool can_wrap_after{false};
    bool whitespace{false};
    bool newline{false};
    bool soft_hyphen{false};
    bool right_to_left{false};
};

struct LayoutLine {
    std::uint32_t text_start{0};
    std::uint32_t length{0};
    std::uint32_t trailing_whitespace{0};
    std::uint32_t newline{0};
    float top{0};
    float height{0};
    float baseline{0};  ///< from the line top
    float left{0};
    float width{0};     ///< excluding trailing whitespace
    float width_with_whitespace{0};
    bool trimmed{false};
};

struct TextLayoutResult {
    std::vector<LayoutGlyphRun> runs;
    std::vector<LayoutCluster> clusters;
    std::vector<LayoutLine> lines;
    float left{0};
    float top{0};
    float width{0};
    float width_with_whitespace{0};
    float height{0};
    std::uint32_t max_bidi_depth{1};
    /// DWRITE_OVERHANG_METRICS: ink beyond the layout box (positive = outside).
    float overhang_left{0};
    float overhang_top{0};
    float overhang_right{0};
    float overhang_bottom{0};
    float min_width{0};
};

/// Why a layout could not be produced. Each is a contract Vitreo does not
/// implement, reported by the caller as UNIMPLEMENTED with this name.
enum class TextLayoutGap {
    None,
    VerticalFlow,       ///< reading/flow direction other than horizontal TTB
    NativeTypesetter,   ///< CoreText could not create the typesetter or a line
    UnresolvableFont,   ///< no host face for a style, even after fallback
};

[[nodiscard]] std::optional<TextLayoutResult> layout_text(
    const TextLayoutParams& params, TextLayoutGap* gap = nullptr);

/// The face CoreText chose (fallback) as a DirectWrite face description.
[[nodiscard]] MatchedFont describe_native_font(const void* ct_font);

}  // namespace vitreo::winapi::dwrite
