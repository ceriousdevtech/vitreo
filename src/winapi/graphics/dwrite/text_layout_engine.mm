// SPDX-License-Identifier: LGPL-2.1-only
// See text_layout_engine.hpp. Every DirectWrite rule below that Learn does not
// state is quoted from tools/oracle_probe/dwrite_text_layout_probe.expected.
#include "winapi/graphics/dwrite/text_layout_engine.hpp"
#include "winapi/graphics/dwrite/font_backend_native.h"

#import <AppKit/AppKit.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace vitreo::winapi::dwrite {
namespace {

constexpr std::uint32_t kAlignLeading = 0, kAlignTrailing = 1, kAlignCenter = 2, kAlignJustified = 3;
constexpr std::uint32_t kParagraphFar = 1, kParagraphCenter = 2;
constexpr std::uint32_t kWrapNone = 1, kWrapWholeWord = 3, kWrapCharacter = 4;
constexpr std::uint32_t kSpacingUniform = 1, kSpacingProportional = 2;
constexpr std::uint32_t kTrimNone = 0, kTrimWord = 2;

template <typename T>
struct Cf {
    T ref{nullptr};
    Cf() = default;
    explicit Cf(T value) : ref(value) {}
    Cf(const Cf&) = delete;
    Cf& operator=(const Cf&) = delete;
    ~Cf() { if (ref) CFRelease(ref); }
    T get() const { return ref; }
    explicit operator bool() const { return ref != nullptr; }
};

// dwrite_text_layout_probe: \n \r \v \f U+0085 U+2028 U+2029 end a line.
bool is_line_separator(char16_t c) {
    return c == 0x0A || c == 0x0D || c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 ||
           c == 0x2029;
}

/// Unicode White_Space, which DWRITE_CLUSTER_METRICS::isWhitespace reports.
bool is_whitespace(char16_t c) {
    return c == 0x09 || is_line_separator(c) || c == 0x20 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}

/// Trailing whitespace a line excludes from `width`: probe "\t" alone has
/// trailingWhitespaceLength 0, so a tab is whitespace but not trailing.
bool is_trailing_whitespace(char16_t c) { return c != 0x09 && is_whitespace(c); }

struct Natural {
    float above{0};  ///< ascent + line gap: DEFAULT baseline
    float below{0};  ///< descent
};

/// DEFAULT spacing: height (ascent+descent+lineGap)*s, baseline
/// (ascent+lineGap)*s -- the line gap sits above the ascent.
Natural natural_metrics(const MatchedFont& font, float size) {
    const float units = font.metrics.design_units_per_em != 0 ? font.metrics.design_units_per_em : 1;
    const float s = size / units;
    return {(font.metrics.ascent + font.metrics.line_gap) * s, font.metrics.descent * s};
}

CTFontRef native_font(const TextLayoutStyle& style) {
    CTFontRef base = nullptr;
    if (style.font_resolved) {
        base = create_native_font(style.font);
    } else {
        // A family the collection lacks still lays out: DirectWrite falls back
        // to the system UI face (probe: a nonexistent family draws with it).
        CFStringRef language = nullptr;
        if (!style.locale.empty()) {
            language = CFStringCreateWithCharacters(
                nullptr, reinterpret_cast<const UniChar*>(style.locale.data()),
                static_cast<CFIndex>(style.locale.size()));
        }
        base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, style.size, language);
        if (language) CFRelease(language);
    }
    if (!base) return nullptr;
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!style.features.empty()) {
        CFMutableArrayRef settings = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
        for (const auto& [tag, value] : style.features) {
            const char chars[4] = {static_cast<char>(tag & 0xFF), static_cast<char>((tag >> 8) & 0xFF),
                                   static_cast<char>((tag >> 16) & 0xFF), static_cast<char>(tag >> 24)};
            CFStringRef tag_string = CFStringCreateWithBytes(
                nullptr, reinterpret_cast<const UInt8*>(chars), 4, kCFStringEncodingASCII, false);
            const int v = static_cast<int>(value);
            CFNumberRef number = CFNumberCreate(nullptr, kCFNumberIntType, &v);
            const void* keys[] = {kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue};
            const void* values[] = {tag_string, number};
            CFDictionaryRef setting = CFDictionaryCreate(nullptr, keys, values, 2,
                                                         &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
            CFArrayAppendValue(settings, setting);
            CFRelease(setting);
            CFRelease(number);
            CFRelease(tag_string);
        }
        CFDictionarySetValue(attributes, kCTFontFeatureSettingsAttribute, settings);
        CFRelease(settings);
    }
    CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes(attributes);
    CFRelease(attributes);
    CTFontRef font = CTFontCreateCopyWithAttributes(base, style.size, nullptr, descriptor);
    CFRelease(descriptor);
    CFRelease(base);
    return font;
}

struct InlineBox {
    float width, ascent, descent;
};
CGFloat inline_ascent(void* ref) { return static_cast<InlineBox*>(ref)->ascent; }
CGFloat inline_descent(void* ref) { return static_cast<InlineBox*>(ref)->descent; }
CGFloat inline_width(void* ref) { return static_cast<InlineBox*>(ref)->width; }
void inline_dealloc(void* ref) { delete static_cast<InlineBox*>(ref); }

/// UAX #14 opportunities: breaks[i] is true when a line may break before
/// unit i (i in 1..n). AppKit's lineBreakBeforeIndex implements UAX #14.
std::vector<bool> break_opportunities(NSAttributedString* string, std::size_t n) {
    std::vector<bool> breaks(n + 1, false);
    if (n == 0) return breaks;
    breaks[n] = true;  // LB3: always break at the end of text
    NSUInteger position = n;
    while (position > 0) {
        const NSUInteger at = [string lineBreakBeforeIndex:position withinRange:NSMakeRange(0, n)];
        if (at == NSNotFound || at == 0 || at > position) break;
        breaks[at] = true;
        position = at - 1;
    }
    return breaks;
}

struct LineSpan {
    std::uint32_t start{0};
    std::uint32_t content{0};  ///< units before the newline sequence
    std::uint32_t newline{0};
};

}  // namespace

MatchedFont describe_native_font(const void* ct_font) {
    return describe_ct_font(static_cast<CTFontRef>(const_cast<void*>(ct_font)));
}

std::optional<TextLayoutResult> layout_text(const TextLayoutParams& p, TextLayoutGap* gap) {
    const auto fail = [&](TextLayoutGap reason) -> std::optional<TextLayoutResult> {
        if (gap) *gap = reason;
        return std::nullopt;
    };
    if (gap) *gap = TextLayoutGap::None;
    if (p.flow_direction != 0 || p.reading_direction > 1) return fail(TextLayoutGap::VerticalFlow);
    const std::size_t n = p.text.size();
    const bool rtl_paragraph = p.reading_direction == 1;
    const auto style_at = [&](std::size_t i) -> const TextLayoutStyle& {
        return p.styles[i < p.style_of.size() ? p.style_of[i] : p.base_style];
    };

    // --- Attributed string ---------------------------------------------------
    @autoreleasepool {
    Cf<CFStringRef> cf_text(CFStringCreateWithCharacters(
        nullptr, reinterpret_cast<const UniChar*>(p.text.data()), static_cast<CFIndex>(n)));
    Cf<CFMutableAttributedStringRef> attributed(CFAttributedStringCreateMutable(nullptr, 0));
    if (!cf_text || !attributed) return fail(TextLayoutGap::NativeTypesetter);
    CFAttributedStringReplaceString(attributed.get(), CFRangeMake(0, 0), cf_text.get());
    CFAttributedStringBeginEditing(attributed.get());
    std::vector<Cf<CTFontRef>> fonts(p.styles.size());
    for (std::size_t i = 0; i < p.styles.size(); ++i) {
        fonts[i].ref = native_font(p.styles[i]);
        if (!fonts[i]) return fail(TextLayoutGap::UnresolvableFont);
    }
    for (std::size_t start = 0; start < n;) {
        std::size_t end = start + 1;
        while (end < n && p.style_of[end] == p.style_of[start]) ++end;
        const TextLayoutStyle& style = p.styles[p.style_of[start]];
        const CFRange range = CFRangeMake(static_cast<CFIndex>(start), static_cast<CFIndex>(end - start));
        CFAttributedStringSetAttribute(attributed.get(), range, kCTFontAttributeName,
                                       fonts[p.style_of[start]].get());
        if (!style.locale.empty()) {
            Cf<CFStringRef> language(CFStringCreateWithCharacters(
                nullptr, reinterpret_cast<const UniChar*>(style.locale.data()),
                static_cast<CFIndex>(style.locale.size())));
            CFAttributedStringSetAttribute(attributed.get(), range, kCTLanguageAttributeName,
                                           language.get());
        }
        if (style.inline_object) {
            // The whole range is one object box: the first unit carries its
            // metrics, the rest are zero-width.
            for (std::size_t i = start; i < end; ++i) {
                const InlineObjectMetrics& io = style.inline_metrics;
                auto* box = new InlineBox{i == start ? io.width : 0, i == start ? io.baseline : 0,
                                          i == start ? io.height - io.baseline : 0};
                CTRunDelegateCallbacks callbacks{kCTRunDelegateCurrentVersion, inline_dealloc,
                                                 inline_ascent, inline_descent, inline_width};
                Cf<CTRunDelegateRef> delegate(CTRunDelegateCreate(&callbacks, box));
                CFAttributedStringSetAttribute(attributed.get(), CFRangeMake(static_cast<CFIndex>(i), 1),
                                               kCTRunDelegateAttributeName, delegate.get());
            }
        }
        start = end;
    }
    {
        const CTWritingDirection direction =
            rtl_paragraph ? kCTWritingDirectionRightToLeft : kCTWritingDirectionLeftToRight;
        const CGFloat tab = p.incremental_tab_stop > 0 ? p.incremental_tab_stop
                                                      : style_at(0).size * 4;  // Learn: 4 x em
        Cf<CFArrayRef> no_stops(CFArrayCreate(nullptr, nullptr, 0, &kCFTypeArrayCallBacks));
        const CFArrayRef stops = no_stops.get();
        CTParagraphStyleSetting settings[] = {
            {kCTParagraphStyleSpecifierBaseWritingDirection, sizeof(direction), &direction},
            {kCTParagraphStyleSpecifierDefaultTabInterval, sizeof(tab), &tab},
            {kCTParagraphStyleSpecifierTabStops, sizeof(stops), &stops},
        };
        Cf<CTParagraphStyleRef> paragraph(CTParagraphStyleCreate(settings, 3));
        CFAttributedStringSetAttribute(attributed.get(), CFRangeMake(0, static_cast<CFIndex>(n)),
                                       kCTParagraphStyleAttributeName, paragraph.get());
    }
    CFAttributedStringEndEditing(attributed.get());
    Cf<CTTypesetterRef> typesetter(CTTypesetterCreateWithAttributedString(attributed.get()));
    if (!typesetter && n != 0) return fail(TextLayoutGap::NativeTypesetter);
    NSAttributedString* ns_string = (__bridge NSAttributedString*)attributed.get();
    const std::vector<bool> breaks = break_opportunities(ns_string, n);

    // --- Line breaking -------------------------------------------------------
    const bool wrap = p.word_wrapping != kWrapNone && std::isfinite(p.max_width);
    std::vector<LineSpan> spans;
    for (std::size_t para = 0; para < n || (n == 0 && spans.empty());) {
        std::size_t content_end = para;
        while (content_end < n && !is_line_separator(p.text[content_end])) ++content_end;
        std::uint32_t newline = 0;
        if (content_end < n) {
            newline = (p.text[content_end] == 0x0D && content_end + 1 < n && p.text[content_end + 1] == 0x0A) ? 2 : 1;
        }
        std::size_t pos = para;
        do {
            std::size_t count = content_end - pos;
            if (wrap && count != 0) {
                const CFIndex suggested = p.word_wrapping == kWrapCharacter
                    ? CTTypesetterSuggestClusterBreak(typesetter.get(), static_cast<CFIndex>(pos), p.max_width)
                    : CTTypesetterSuggestLineBreak(typesetter.get(), static_cast<CFIndex>(pos), p.max_width);
                count = std::clamp<std::size_t>(static_cast<std::size_t>(std::max<CFIndex>(suggested, 1)), 1, count);
                if (p.word_wrapping == kWrapWholeWord) {
                    // Never break inside a word: run on to the next opportunity.
                    while (pos + count < content_end && !breaks[pos + count]) ++count;
                } else if (p.word_wrapping != kWrapCharacter) {
                    // A break CoreText placed inside a word is DirectWrite's
                    // emergency break; one at an opportunity is kept as is.
                }
                // Trailing whitespace stays on the line (probe: lines keep
                // their trailing spaces in `length`).
                while (pos + count < content_end && is_trailing_whitespace(p.text[pos + count])) ++count;
            }
            const bool last = pos + count >= content_end;
            spans.push_back({static_cast<std::uint32_t>(pos), static_cast<std::uint32_t>(count),
                             last ? newline : 0});
            pos += count;
        } while (pos < content_end);
        para = content_end + newline;
        if (n == 0) break;
    }
    // Text ending in a newline (or empty text) has a final empty line at full
    // height (probe: "\n" and "Hello\n" both report lineCount 2).
    if (n == 0 || is_line_separator(p.text[n - 1])) {
        if (n != 0) spans.push_back({static_cast<std::uint32_t>(n), 0, 0});
    }

    // --- Shaping, line metrics ------------------------------------------------
    TextLayoutResult result;
    std::uint32_t max_level = 0;
    for (std::size_t li = 0; li < spans.size(); ++li) {
        const LineSpan& span = spans[li];
        LayoutLine line;
        line.text_start = span.start;
        line.length = span.content + span.newline;
        line.newline = span.newline;
        Natural natural{};
        const auto include = [&](const Natural& value) {
            natural.above = std::max(natural.above, value.above);
            natural.below = std::max(natural.below, value.below);
        };
        const std::size_t first_run = result.runs.size();
        float pen = 0;
        if (span.content != 0) {
            Cf<CTLineRef> ct_line(CTTypesetterCreateLine(
                typesetter.get(), CFRangeMake(span.start, span.content)));
            if (!ct_line) return fail(TextLayoutGap::NativeTypesetter);
            CFArrayRef ct_runs = CTLineGetGlyphRuns(ct_line.get());
            for (CFIndex r = 0; r < CFArrayGetCount(ct_runs); ++r) {
                CTRunRef ct_run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(ct_runs, r));
                const CFIndex count = CTRunGetGlyphCount(ct_run);
                const CFRange range = CTRunGetStringRange(ct_run);
                if (range.length <= 0) continue;
                const bool rtl = (CTRunGetStatus(ct_run) & kCTRunStatusRightToLeft) != 0;
                const TextLayoutStyle& style = style_at(static_cast<std::size_t>(range.location));
                LayoutGlyphRun run;
                run.text_start = static_cast<std::uint32_t>(range.location);
                run.text_length = static_cast<std::uint32_t>(range.length);
                run.style = p.style_of[static_cast<std::size_t>(range.location)];
                run.size = style.size;
                run.line = static_cast<std::uint32_t>(li);
                run.bidi_level = rtl_paragraph ? (rtl ? 1u : 2u) : (rtl ? 1u : 0u);
                max_level = std::max(max_level, run.bidi_level);
                std::vector<CGGlyph> glyphs(static_cast<std::size_t>(count));
                std::vector<CGPoint> positions(static_cast<std::size_t>(count));
                std::vector<CGSize> advances(static_cast<std::size_t>(count));
                std::vector<CFIndex> indices(static_cast<std::size_t>(count));
                CTRunGetGlyphs(ct_run, CFRangeMake(0, 0), glyphs.data());
                CTRunGetPositions(ct_run, CFRangeMake(0, 0), positions.data());
                CTRunGetAdvances(ct_run, CFRangeMake(0, 0), advances.data());
                CTRunGetStringIndices(ct_run, CFRangeMake(0, 0), indices.data());
                float width = 0;
                for (const CGSize& a : advances) width += static_cast<float>(a.width);
                run.width = width;
                const float left = count ? static_cast<float>(positions[0].x) : pen;
                if (style.inline_object) {
                    run.inline_object = true;
                    run.font = style.font;
                    run.width = style.inline_metrics.width;
                    run.baseline_x = left;
                    run.inline_baseline = style.inline_metrics.baseline;
                    run.cluster_map.assign(run.text_length, 0);
                    include({style.inline_metrics.baseline,
                             style.inline_metrics.height - style.inline_metrics.baseline});
                } else {
                    CTFontRef run_font = static_cast<CTFontRef>(
                        CFDictionaryGetValue(CTRunGetAttributes(ct_run), kCTFontAttributeName));
                    const bool fallback = run_font != fonts[run.style].get();
                    run.font = (fallback || !style.font_resolved) ? describe_ct_font(run_font) : style.font;
                    run.simulations = fallback ? 0 : style.simulations;
                    include(natural_metrics(run.font, run.size));
                    // DirectWrite runs are in logical order; an RTL run's
                    // origin is its right edge and its pen moves left.
                    std::vector<std::size_t> order(static_cast<std::size_t>(count));
                    for (std::size_t k = 0; k < order.size(); ++k) order[k] = rtl ? order.size() - 1 - k : k;
                    run.baseline_x = rtl ? left + width : left;
                    float advance_sum = 0;
                    std::vector<CFIndex> logical_index;
                    for (std::size_t j = 0; j < order.size(); ++j) {
                        const std::size_t k = order[j];
                        const float advance = static_cast<float>(advances[k].width);
                        run.glyphs.push_back(glyphs[k]);
                        run.advances.push_back(advance);
                        const float expected = rtl ? run.baseline_x - advance_sum - advance
                                                   : run.baseline_x + advance_sum;
                        const float actual = static_cast<float>(positions[k].x);
                        run.offsets.push_back({rtl ? expected - actual : actual - expected,
                                               static_cast<float>(positions[k].y)});
                        advance_sum += advance;
                        logical_index.push_back(indices[k]);
                    }
                    // clusterMap: each unit -> first glyph of its cluster.
                    run.cluster_map.assign(run.text_length, 0);
                    for (std::uint32_t u = 0; u < run.text_length; ++u) {
                        const CFIndex unit = range.location + u;
                        CFIndex best = -1;
                        std::size_t glyph = 0;
                        for (std::size_t j = 0; j < logical_index.size(); ++j) {
                            if (logical_index[j] <= unit && logical_index[j] > best) {
                                best = logical_index[j];
                                glyph = j;
                            }
                        }
                        while (glyph > 0 && logical_index[glyph - 1] == best) --glyph;
                        run.cluster_map[u] = static_cast<std::uint16_t>(glyph);
                    }
                }
                pen = left + run.width;
                result.runs.push_back(std::move(run));
            }
        }
        // The newline sequence draws as a glyph-less run (probe: glyphCount 0,
        // stringLength 1) and sizes an empty line with its formatting.
        if (span.newline != 0 || span.content == 0) {
            const std::uint32_t at = span.start + span.content;
            const TextLayoutStyle& style = style_at(at);
            LayoutGlyphRun run;
            run.text_start = at;
            run.text_length = span.newline;
            run.style = at < p.style_of.size() ? p.style_of[at] : p.base_style;
            run.size = style.size;
            run.line = static_cast<std::uint32_t>(li);
            run.bidi_level = rtl_paragraph ? 1 : 0;
            if (style.font_resolved) {
                run.font = style.font;
            } else {
                run.font = describe_ct_font(fonts[run.style].get());
            }
            run.simulations = style.simulations;
            run.baseline_x = pen;
            run.cluster_map.assign(run.text_length, 0);
            if (span.content == 0 || natural.above == 0) include(natural_metrics(run.font, run.size));
            if (span.newline != 0) result.runs.push_back(std::move(run));
        }
        const float natural_height = natural.above + natural.below;
        if (p.line_spacing_method == kSpacingUniform) {
            line.height = p.line_spacing;
            line.baseline = p.baseline;
        } else if (p.line_spacing_method == kSpacingProportional) {
            line.height = natural_height * p.line_spacing;
            line.baseline = natural.above * p.baseline;
        } else {
            line.height = natural_height;
            line.baseline = natural.above;
        }
        // Width with and without the trailing whitespace (newline included).
        float total = 0, trailing = 0;
        bool in_tail = true;
        for (std::size_t r = result.runs.size(); r-- > first_run;) {
            total += result.runs[r].width;
        }
        for (std::uint32_t u = line.length; u-- > 0;) {
            const char16_t c = p.text[line.text_start + u];
            if (!in_tail || !is_trailing_whitespace(c)) {
                in_tail = false;
                continue;
            }
            ++line.trailing_whitespace;
        }
        line.width_with_whitespace = total;
        (void)trailing;
        result.lines.push_back(line);
        (void)first_run;
    }

    // --- Clusters ---------------------------------------------------------
    for (const LayoutGlyphRun& run : result.runs) {
        const bool rtl = (run.bidi_level & 1) != 0;
        float advance_before = 0;
        for (std::uint32_t u = 0; u < run.text_length;) {
            std::uint32_t end = u + 1;
            while (end < run.text_length && run.cluster_map[end] == run.cluster_map[u]) ++end;
            const std::size_t g0 = run.cluster_map[u];
            const std::size_t g1 = end < run.text_length ? run.cluster_map[end] : run.glyphs.size();
            float width = 0;
            if (run.inline_object) {
                width = u == 0 ? run.width : 0;
            } else {
                for (std::size_t g = g0; g < g1 && g < run.advances.size(); ++g) width += run.advances[g];
            }
            LayoutCluster cluster;
            cluster.text_start = run.text_start + u;
            cluster.length = end - u;
            cluster.width = width;
            cluster.line = run.line;
            cluster.bidi_level = run.bidi_level;
            cluster.right_to_left = rtl;
            cluster.x = rtl ? run.baseline_x - advance_before - width : run.baseline_x + advance_before;
            const char16_t c = p.text[cluster.text_start];
            cluster.whitespace = is_whitespace(c);
            cluster.newline = is_line_separator(c);
            cluster.soft_hyphen = c == 0xAD;
            const std::size_t after = cluster.text_start + cluster.length;
            cluster.can_wrap_after = cluster.newline || (after <= n && breaks[after]);
            advance_before += width;
            result.clusters.push_back(cluster);
            u = end;
        }
    }
    std::sort(result.clusters.begin(), result.clusters.end(),
              [](const LayoutCluster& a, const LayoutCluster& b) { return a.text_start < b.text_start; });
    for (LayoutLine& line : result.lines) {
        float trailing = 0;
        for (const LayoutCluster& c : result.clusters) {
            if (c.text_start >= line.text_start + line.length - line.trailing_whitespace &&
                c.text_start < line.text_start + line.length) {
                trailing += c.width;
            }
        }
        line.width = std::max(0.f, line.width_with_whitespace - trailing);
    }

    // --- Trimming -----------------------------------------------------------
    const bool box_width = std::isfinite(p.max_width);
    if (p.trimming_granularity != kTrimNone && box_width) {
        for (std::size_t li = 0; li < result.lines.size(); ++li) {
            LayoutLine& line = result.lines[li];
            if (line.width <= p.max_width) continue;
            const float sign = p.has_trimming_sign ? p.trimming_sign.width : 0;
            const float available = p.max_width - sign;
            // Keep whole clusters (or words) from the line start that fit,
            // then drop the whitespace they end with.
            std::uint32_t keep_end = line.text_start;
            float used = 0;
            for (const LayoutCluster& c : result.clusters) {
                if (c.line != li || c.newline) continue;
                if (used + c.width > available) break;
                used += c.width;
                if (p.trimming_granularity != kTrimWord || c.can_wrap_after || c.whitespace)
                    keep_end = c.text_start + c.length;
            }
            while (keep_end > line.text_start && is_whitespace(p.text[keep_end - 1])) --keep_end;
            const std::uint32_t content_end = line.text_start + line.length - line.newline;
            line.trimmed = true;
            // Drop the glyphs and clusters of [keep_end, content_end).
            float kept_width = 0;
            for (auto& c : result.clusters) {
                if (c.line == li && c.text_start < keep_end) kept_width = std::max(kept_width, c.x + c.width);
            }
            std::vector<LayoutGlyphRun> runs;
            for (auto& run : result.runs) {
                if (run.line != li || run.text_start + run.text_length <= keep_end || run.text_length == 0 ||
                    run.text_start >= content_end) {
                    runs.push_back(std::move(run));
                    continue;
                }
                if (run.text_start >= keep_end) continue;
                const std::uint32_t units = keep_end - run.text_start;
                const std::size_t glyph_end = units < run.text_length ? run.cluster_map[units] : run.glyphs.size();
                run.glyphs.resize(glyph_end);
                run.advances.resize(glyph_end);
                run.offsets.resize(glyph_end);
                run.cluster_map.resize(units);
                run.text_length = units;
                runs.push_back(std::move(run));
            }
            result.runs = std::move(runs);
            if (p.has_trimming_sign) {
                LayoutGlyphRun sign_run;
                sign_run.trimming_sign = true;
                sign_run.inline_object = true;
                sign_run.text_start = keep_end;
                sign_run.text_length = content_end - keep_end;
                sign_run.line = static_cast<std::uint32_t>(li);
                sign_run.baseline_x = kept_width;
                sign_run.width = sign;
                sign_run.inline_baseline = p.trimming_sign.baseline;
                result.runs.push_back(std::move(sign_run));
            }
            // Trimmed positions hit-test as one range whose width is the sign.
            std::vector<LayoutCluster> clusters;
            for (auto& c : result.clusters) {
                if (c.line == li && c.text_start >= keep_end && c.text_start < content_end) continue;
                clusters.push_back(c);
            }
            LayoutCluster trimmed;
            trimmed.text_start = keep_end;
            trimmed.length = content_end - keep_end;
            trimmed.width = sign;
            trimmed.x = kept_width;
            trimmed.line = static_cast<std::uint32_t>(li);
            clusters.push_back(trimmed);
            std::sort(clusters.begin(), clusters.end(),
                      [](const LayoutCluster& a, const LayoutCluster& b) { return a.text_start < b.text_start; });
            result.clusters = std::move(clusters);
            line.width = kept_width + sign;
            line.width_with_whitespace = line.width;
        }
    }

    // --- Alignment and vertical placement --------------------------------------
    float widest = 0;
    for (const LayoutLine& line : result.lines) widest = std::max(widest, line.width);
    const float box = box_width ? p.max_width : widest;
    std::uint32_t align = p.text_alignment;
    if (rtl_paragraph && align == kAlignLeading) align = kAlignTrailing;
    else if (rtl_paragraph && align == kAlignTrailing) align = kAlignLeading;
    for (std::size_t li = 0; li < result.lines.size(); ++li) {
        LayoutLine& line = result.lines[li];
        const bool paragraph_end = line.newline != 0 || li + 1 == result.lines.size();
        float factor = 0;
        if (align == kAlignTrailing) factor = 1;
        else if (align == kAlignCenter) factor = 0.5f;
        line.left = (box - line.width) * factor;
        if (p.text_alignment == kAlignJustified && !paragraph_end && box_width) {
            // Spread the slack over the line's inner whitespace clusters.
            std::vector<LayoutCluster*> gaps;
            const std::uint32_t inner_end = line.text_start + line.length - line.trailing_whitespace;
            for (auto& c : result.clusters)
                if (c.line == li && c.whitespace && !c.newline && c.text_start < inner_end) gaps.push_back(&c);
            if (!gaps.empty()) {
                const float extra = (box - line.width) / static_cast<float>(gaps.size());
                for (auto& run : result.runs) {
                    if (run.line != li || run.inline_object) continue;
                    float shift = 0;
                    for (std::uint32_t u = 0; u < run.text_length; ++u) {
                        const char16_t c = p.text[run.text_start + u];
                        if (is_whitespace(c) && !is_line_separator(c) && run.text_start + u < inner_end) {
                            run.advances[run.cluster_map[u]] += extra;
                            shift += extra;
                        }
                    }
                    run.width += shift;
                }
                float shift = 0;
                for (auto& c : result.clusters) {
                    if (c.line != li) continue;
                    c.x += shift;
                    if (c.whitespace && !c.newline && c.text_start < inner_end) {
                        c.width += extra;
                        shift += extra;
                    }
                }
                // Runs after a widened run move right by the space it gained.
                float run_shift = 0;
                for (auto& run : result.runs) {
                    if (run.line != li) continue;
                    run.baseline_x += run_shift;
                    float gained = 0;
                    for (std::uint32_t u = 0; u < run.text_length; ++u) {
                        const char16_t c = p.text[run.text_start + u];
                        if (is_whitespace(c) && !is_line_separator(c) && run.text_start + u < inner_end) gained += extra;
                    }
                    run_shift += gained;
                }
                line.width = box;
                line.width_with_whitespace = std::max(line.width_with_whitespace, box);
            }
        }
    }
    float height = 0;
    for (LayoutLine& line : result.lines) {
        line.top = height;
        height += line.height;
    }
    const float box_height = std::isfinite(p.max_height) ? p.max_height : height;
    float vertical = 0;
    if (p.paragraph_alignment == kParagraphFar) vertical = 1;
    else if (p.paragraph_alignment == kParagraphCenter) vertical = 0.5f;
    result.top = (box_height - height) * vertical;
    // Trimming by height: lines after the first that end below the box
    // report height and baseline 0 and isTrimmed (probe), and do not draw.
    if (p.trimming_granularity != kTrimNone && std::isfinite(p.max_height)) {
        for (std::size_t li = 1; li < result.lines.size(); ++li) {
            LayoutLine& line = result.lines[li];
            if (line.top + line.height <= p.max_height) continue;
            line.height = 0;
            line.baseline = 0;
            line.trimmed = true;
            result.runs.erase(std::remove_if(result.runs.begin(), result.runs.end(),
                                             [&](const LayoutGlyphRun& r) { return r.line == li; }),
                              result.runs.end());
        }
        height = 0;
        for (LayoutLine& line : result.lines) {
            line.top = height;
            height += line.height;
        }
    }
    result.height = height;
    float left = std::numeric_limits<float>::infinity();
    for (std::size_t li = 0; li < result.lines.size(); ++li) {
        LayoutLine& line = result.lines[li];
        line.top += result.top;
        left = std::min(left, line.left);
        result.width = std::max(result.width, line.width);
        result.width_with_whitespace = std::max(result.width_with_whitespace, line.width_with_whitespace);
    }
    result.left = result.lines.empty() ? 0 : left;
    for (auto& run : result.runs) {
        const LayoutLine& line = result.lines[run.line];
        run.baseline_x += line.left;
        run.baseline_y = line.top + line.baseline;
    }
    for (auto& c : result.clusters) c.x += result.lines[c.line].left;
    result.max_bidi_depth = max_level + 1;

    // --- Minimum width and overhang ----------------------------------------
    float word = 0;
    for (const LayoutCluster& c : result.clusters) {
        if (!c.whitespace) word += c.width;
        result.min_width = std::max(result.min_width, word);
        if (c.can_wrap_after) word = 0;
    }
    CGRect ink = CGRectNull;
    for (const auto& run : result.runs) {
        if (run.inline_object || run.glyphs.empty()) continue;
        TextLayoutStyle style = p.styles[run.style];
        style.font = run.font;
        style.font_resolved = true;
        Cf<CTFontRef> font(native_font(style));
        if (!font) continue;
        std::vector<CGGlyph> glyphs(run.glyphs.begin(), run.glyphs.end());
        std::vector<CGRect> boxes(glyphs.size());
        CTFontGetBoundingRectsForGlyphs(font.get(), kCTFontOrientationHorizontal, glyphs.data(),
                                        boxes.data(), static_cast<CFIndex>(glyphs.size()));
        const bool rtl = (run.bidi_level & 1) != 0;
        float pen = 0;
        for (std::size_t g = 0; g < glyphs.size(); ++g) {
            if (CGRectIsEmpty(boxes[g])) {
                pen += run.advances[g];
                continue;
            }
            const float origin = rtl ? run.baseline_x - pen - run.advances[g] - run.offsets[g].advance_offset
                                     : run.baseline_x + pen + run.offsets[g].advance_offset;
            const CGRect placed = CGRectMake(origin + boxes[g].origin.x,
                                             run.baseline_y - run.offsets[g].ascender_offset -
                                                 (boxes[g].origin.y + boxes[g].size.height),
                                             boxes[g].size.width, boxes[g].size.height);
            ink = CGRectIsNull(ink) ? placed : CGRectUnion(ink, placed);
            pen += run.advances[g];
        }
    }
    if (!CGRectIsNull(ink)) {
        // Measured against the layout box, not the text box (probe).
        result.overhang_left = static_cast<float>(-CGRectGetMinX(ink));
        result.overhang_top = static_cast<float>(-CGRectGetMinY(ink));
        result.overhang_right = static_cast<float>(CGRectGetMaxX(ink)) - p.max_width;
        result.overhang_bottom = static_cast<float>(CGRectGetMaxY(ink)) - p.max_height;
    } else {
        result.overhang_left = -result.left;
        result.overhang_top = -result.top;
        result.overhang_right = result.left + result.width - p.max_width;
        result.overhang_bottom = result.top + result.height - p.max_height;
    }
    return result;
    }  // @autoreleasepool
}

}  // namespace vitreo::winapi::dwrite
