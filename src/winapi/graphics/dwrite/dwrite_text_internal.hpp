// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dwrite_text_internal.hpp
/// @brief IDWriteTextFormat / IDWriteTextLayout / ellipsis-sign state.

#include "winapi/graphics/dwrite/dwrite_internal.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

/// Formatting a text range carries (IDWriteTextLayout range setters).
/// Every GuestAddr here is a COM reference this state holds.
struct DWriteManager::TextStyleSpec {
    GuestAddr collection{0};
    std::u16string family;
    u32 weight{400};
    u32 style{0};
    u32 stretch{5};
    float size{0};
    std::u16string locale;
    bool underline{false};
    bool strikethrough{false};
    GuestAddr effect{0};
    GuestAddr inline_object{0};
    GuestAddr typography{0};
    bool operator==(const TextStyleSpec&) const = default;
};

struct DWriteManager::TextState {
    // IDWriteTextFormat.
    TextStyleSpec format;
    u32 text_alignment{0};
    u32 paragraph_alignment{0};
    u32 word_wrapping{0};
    u32 reading_direction{0};
    u32 flow_direction{0};
    float incremental_tab_stop{0};
    u32 trimming_granularity{0};
    u32 trimming_delimiter{0};
    u32 trimming_delimiter_count{0};
    GuestAddr trimming_sign{0};
    u32 line_spacing_method{0};
    float line_spacing{0};
    float baseline{0};

    // IDWriteTextLayout.
    std::u16string text;
    float max_width{0};
    float max_height{0};
    /// Range formatting over the whole UINT32 position space: run i covers
    /// [runs[i].start, runs[i+1].start), the last one through 0xFFFFFFFF.
    /// Ranges past the text are stored as given (dwrite_text_layout_probe:
    /// SetFontFamilyName({8,100}) on 11 units keeps {8,3} and {11,97}).
    struct StyleRun {
        u32 start{0};
        TextStyleSpec spec;
    };
    std::vector<StyleRun> runs;
    [[nodiscard]] const TextStyleSpec& spec_at(u32 position) const {
        auto it = std::upper_bound(runs.begin(), runs.end(), position,
                                   [](u32 p, const StyleRun& r) { return p < r.start; });
        return std::prev(it)->spec;
    }
    bool gdi_compatible{false};
    float pixels_per_dip{1};
    std::array<float, 6> transform{1, 0, 0, 1, 0, 0};
    bool use_gdi_natural{false};
    std::optional<dwrite::TextLayoutResult> formatted;
    u32 format_error{0};

    // Ellipsis trimming sign: the format it measures with.
    std::shared_ptr<TextState> sign_format;

    void invalidate() { formatted.reset(); format_error = 0; }
};

}  // namespace vitreo::winapi
