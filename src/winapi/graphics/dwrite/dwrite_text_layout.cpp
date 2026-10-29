// SPDX-License-Identifier: LGPL-2.1-only
// IDWriteTextLayout formatting, metrics, hit testing and Draw, and the
// ellipsis trimming sign (IDWriteInlineObject). Rules Learn leaves open are
// from tools/oracle_probe/dwrite_text_layout_probe.expected.
#include "winapi/graphics/dwrite/dwrite_text_internal.hpp"

#include "core/memory/guest_memory.hpp"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>

namespace vitreo::winapi {
using namespace dwrite_text_contract;
namespace {

const char* gap_name(dwrite::TextLayoutGap gap) {
    switch (gap) {
    case dwrite::TextLayoutGap::VerticalFlow: return "vertical reading/flow direction";
    case dwrite::TextLayoutGap::NativeTypesetter: return "CoreText typesetter failure";
    case dwrite::TextLayoutGap::UnresolvableFont: return "no host face for a style";
    case dwrite::TextLayoutGap::None: break;
    }
    return "none";
}

void report_gap(const char* what) {
    static std::mutex mutex;
    static std::set<std::string> once;
    std::lock_guard lock(mutex);
    if (once.insert(what).second) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED dwrite.dll!IDWriteTextLayout: %s\n", what);
    }
}

u32 float_bits(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

}  // namespace

u32 DWriteManager::format_text(TextState& state) {
    if (state.formatted) return S_OK;
    if (state.format_error != 0) return state.format_error;
    dwrite::TextLayoutParams params;
    params.text = state.text;
    params.max_width = state.max_width;
    params.max_height = state.max_height;
    params.text_alignment = state.text_alignment;
    params.paragraph_alignment = state.paragraph_alignment;
    params.word_wrapping = state.word_wrapping;
    params.reading_direction = state.reading_direction;
    params.flow_direction = state.flow_direction;
    params.incremental_tab_stop = state.incremental_tab_stop;
    params.line_spacing_method = state.line_spacing_method;
    params.line_spacing = state.line_spacing;
    params.baseline = state.baseline;
    params.trimming_granularity = state.trimming_granularity;
    params.trimming_delimiter = state.trimming_delimiter;
    params.trimming_delimiter_count = state.trimming_delimiter_count;
    const auto fail = [&](u32 hr, const char* what) {
        report_gap(what);
        state.format_error = hr;
        return hr;
    };
    if (state.trimming_delimiter != 0) {
        // The delimiter keeps the text after its Nth occurrence from the end
        // visible (path ellipsis); the formatter trims from the end only.
        return fail(E_NOTIMPL, "DWRITE_TRIMMING delimiter");
    }
    // Inline object / sign metrics come from the guest object itself.
    const auto inline_metrics = [&](GuestAddr object, dwrite::InlineObjectMetrics& out) -> bool {
        if (auto ours = m_objects.find(object);
            ours != m_objects.end() && ours->second.type == ObjectType::TrimmingSign) {
            auto& sign = *ours->second.text->sign_format;
            if (format_text(sign) != S_OK || sign.formatted->lines.empty()) return false;
            out.width = sign.formatted->width;
            out.height = sign.formatted->lines[0].height;
            out.baseline = sign.formatted->lines[0].baseline;
            return true;
        }
        if (!ensure_storage()) return false;
        const GuestAddr scratch = m_callback_scratch;
        const u64 metrics_args[]{scratch};
        const auto hr = call_guest_method(object, 4, metrics_args, 1);  // GetMetrics
        if (!hr || (static_cast<u32>(*hr) & 0x80000000u) != 0) return false;
        std::array<u32, 4> raw{};
        if (m_memory.read_bytes(scratch, reinterpret_cast<Byte*>(raw.data()), sizeof(raw)).is_err()) return false;
        out.width = float_from_bits(raw[0]);
        out.height = float_from_bits(raw[1]);
        out.baseline = float_from_bits(raw[2]);
        const u64 break_args[]{scratch, scratch + 4};
        const auto breaks = call_guest_method(object, 6, break_args, 2);  // GetBreakConditions
        if (breaks && (static_cast<u32>(*breaks) & 0x80000000u) == 0) {
            out.break_before = m_memory.read_u32(scratch).value_or(0);
            out.break_after = m_memory.read_u32(scratch + 4).value_or(0);
        }
        return true;
    };
    if (state.trimming_sign != 0) {
        params.has_trimming_sign = true;
        if (!inline_metrics(state.trimming_sign, params.trimming_sign)) {
            return fail(E_FAIL, "trimming sign GetMetrics failed");
        }
    }
    std::vector<TextStyleSpec> specs;
    const auto intern = [&](const TextStyleSpec& spec) {
        auto found = std::find(specs.begin(), specs.end(), spec);
        if (found != specs.end()) return static_cast<u32>(found - specs.begin());
        specs.push_back(spec);
        return static_cast<u32>(specs.size() - 1);
    };
    params.style_of.resize(state.text.size());
    for (usize i = 0; i < state.text.size(); ++i) {
        params.style_of[i] = intern(state.spec_at(static_cast<u32>(i)));
    }
    params.base_style = intern(state.spec_at(static_cast<u32>(state.text.size())));
    for (const TextStyleSpec& spec : specs) {
        dwrite::TextLayoutStyle style;
        u32 simulations = 0;
        auto font = resolve_text_font(spec.collection, spec.family, spec.weight, spec.style,
                                      spec.stretch, simulations);
        if (!font) {
            // A family the collection lacks falls back to the system UI
            // face (probe: a nonexistent family still formats and draws).
            font = dwrite::match_system_ui_font(spec.weight, spec.stretch, spec.style);
            simulations = 0;
            if (font && spec.weight >= 600 && font->weight <= 500) simulations |= DWRITE_FONT_SIMULATIONS_BOLD;
            if (font && spec.style != 0 && font->style == 0) simulations |= DWRITE_FONT_SIMULATIONS_OBLIQUE;
        }
        if (!font) return fail(E_FAIL, "no host face for a text style");
        style.font = std::move(*font);
        style.font_resolved = true;
        style.simulations = simulations;
        style.size = spec.size;
        style.locale = spec.locale;
        style.underline = spec.underline;
        style.strikethrough = spec.strikethrough;
        if (spec.typography != 0) {
            const auto typography = m_objects.find(spec.typography);
            if (typography == m_objects.end() || typography->second.type != ObjectType::Typography) {
                return fail(E_NOTIMPL, "IDWriteTypography not created by this factory");
            }
            style.features = typography->second.typography_features;
        }
        if (spec.inline_object != 0) {
            style.inline_object = true;
            if (!inline_metrics(spec.inline_object, style.inline_metrics)) {
                return fail(E_FAIL, "inline object GetMetrics failed");
            }
        }
        params.styles.push_back(std::move(style));
    }
    dwrite::TextLayoutGap gap{};
    auto formatted = dwrite::layout_text(params, &gap);
    if (!formatted) return fail(E_NOTIMPL, gap_name(gap));
    state.formatted = std::move(formatted);
    return S_OK;
}

namespace {

/// DWRITE_HIT_TEST_METRICS for @p cluster (probe: the rect is the run
/// font's natural height, placed so its natural baseline meets the line's).
struct Hit {
    u32 position{0}, length{0};
    float left{0}, top{0}, width{0}, height{0};
    u32 bidi{0};
    bool text{true}, trimmed{false};
};

float natural_height(const dwrite::MatchedFont& font, float size, float* baseline) {
    const float units = font.metrics.design_units_per_em ? font.metrics.design_units_per_em : 1;
    const float s = size / units;
    if (baseline) *baseline = (font.metrics.ascent + font.metrics.line_gap) * s;
    return (font.metrics.ascent + font.metrics.descent + font.metrics.line_gap) * s;
}

const dwrite::LayoutGlyphRun* run_of(const dwrite::TextLayoutResult& layout, u32 position) {
    for (const auto& run : layout.runs) {
        if (position >= run.text_start && position < run.text_start + std::max(run.text_length, 1u)) return &run;
    }
    return nullptr;
}

Hit cluster_hit(const dwrite::TextLayoutResult& layout, const dwrite::LayoutCluster& c) {
    Hit hit;
    hit.position = c.text_start;
    hit.length = c.length;
    hit.left = c.x;
    hit.width = c.width;
    hit.bidi = c.bidi_level;
    const auto& line = layout.lines[c.line];
    hit.trimmed = line.trimmed && c.width == 0 ? false : false;
    const auto* run = run_of(layout, c.text_start);
    float baseline = line.baseline;
    if (run && !run->inline_object) {
        hit.height = natural_height(run->font, run->size, &baseline);
        hit.top = line.top + line.baseline - baseline;
    } else {
        hit.height = line.height;
        hit.top = line.top;
        hit.text = !(run && run->inline_object);
        hit.trimmed = run && run->trimming_sign;
    }
    return hit;
}

}  // namespace

namespace {

/// One mapped guest buffer for the structures a Draw hands to callbacks.
struct DrawScratch {
    GuestMemory& memory;
    GuestAddr base{0};
    usize size{0};
    usize used{0};
    DrawScratch(GuestMemory& m, usize bytes) : memory(m) {
        size = (std::max<usize>(bytes, 1) + GUEST_PAGE_SIZE - 1) & ~(GUEST_PAGE_SIZE - 1);
        base = memory.find_free_gap(0x6D000000ull, size);
        if (base != 0 && memory.map_region(base, size, MemoryProtection::ReadWrite).is_err()) base = 0;
    }
    ~DrawScratch() {
        if (base != 0) (void)memory.unmap_region(base, size);
    }
    GuestAddr take(usize bytes) {
        used = (used + 7) & ~usize{7};
        if (base == 0 || used + bytes > size) return 0;
        const GuestAddr at = base + used;
        used += bytes;
        return at;
    }
};

struct Underline {
    usize first_run{0};
    float x{0}, width{0}, thickness{0}, offset{0}, run_height{0};
    GuestAddr effect{0};
    std::u16string locale;
    bool strikethrough{false};
};

}  // namespace

namespace {

/// DrawUnderline / DrawStrikethrough geometry for every line (probe):
/// underlines merge across adjacent runs into one call with a
/// width-weighted thickness, the largest rounded offset and the largest
/// capHeight run height; strikethroughs come one per run.
std::vector<Underline> decorations(const DWriteManager::TextState& state,
                                   const dwrite::TextLayoutResult& layout) {
    std::vector<Underline> out;
    for (usize i = 0; i < layout.runs.size(); ++i) {
        const auto& run = layout.runs[i];
        if (run.inline_object || run.glyphs.empty()) continue;
        const auto& spec = state.spec_at(run.text_start);
        const float units = run.font.metrics.design_units_per_em ? run.font.metrics.design_units_per_em : 1;
        const float s = run.size / units;
        const float left = (run.bidi_level & 1) ? run.baseline_x - run.width : run.baseline_x;
        if (spec.underline) {
            const float thickness = run.font.metrics.underline_thickness * s;
            const float offset = std::round(-run.font.metrics.underline_position * s);
            const float height = run.font.metrics.cap_height * s;
            Underline* merge = nullptr;
            for (auto it = out.rbegin(); it != out.rend(); ++it) {
                if (!it->strikethrough) {
                    merge = &*it;
                    break;
                }
            }
            if (merge && layout.runs[merge->first_run].line == run.line &&
                std::abs(merge->x + merge->width - left) < 0.01f && merge->effect == spec.effect) {
                merge->thickness = (merge->thickness * merge->width + thickness * run.width) /
                                   std::max(merge->width + run.width, 1e-6f);
                merge->width += run.width;
                merge->offset = std::max(merge->offset, offset);
                merge->run_height = std::max(merge->run_height, height);
            } else {
                out.push_back({i, left, run.width, thickness, offset, height, spec.effect, spec.locale, false});
            }
        }
        if (spec.strikethrough) {
            out.push_back({i, left, run.width, run.font.metrics.strikethrough_thickness * s,
                           -run.font.metrics.strikethrough_position * s, 0, spec.effect, {}, true});
        }
    }
    return out;
}

}  // namespace

u32 DWriteManager::text_layout_query(Marshaler& m, GuestAddr self, TextState& state, u32 slot) {
    (void)self;
    const bool b64 = m_is_64bit;
    const usize w = b64 ? 1 : 0;
    const auto write_floats = [&](GuestAddr at, std::initializer_list<u32> words) {
        usize i = 0;
        for (const u32 word : words) {
            if (m_memory.write_u32(at + 4 * i++, word).is_err()) return false;
        }
        return true;
    };
    if (slot == text_layout_Draw) {
        const GuestAddr context = m.arg_ptr(1);
        const GuestAddr renderer = m.arg_ptr(2);
        const float x = read_float_argument(m, 3);
        const float y = read_float_argument(m, 4);
        if (renderer == 0) return E_INVALIDARG;
        if (const u32 hr = format_text(state); hr != S_OK) return hr;
        // Probe: Draw first asks the renderer for IDWriteTextRenderer1.
        if (ensure_storage()) {
            const GuestAddr iid = m_callback_scratch + 0x40;
            const GuestAddr out = m_callback_scratch + 0x60;
            if (m_memory.write_bytes(iid, iid_IDWriteTextRenderer1.data(), 16).is_ok() &&
                write_pointer(out, 0)) {
                const u64 qi_args[]{iid, out};
                const auto qi = call_guest_method(renderer, 0, qi_args, 2);
                const auto got = read_pointer(out);
                if (qi && static_cast<u32>(*qi) == S_OK && got.is_ok() && got.value() != 0) {
                    release_guest_object(got.value());
                }
            }
        }
        return render_to(state, context, renderer, x, y);
    }
    if (const u32 hr = format_text(state); hr != S_OK) return hr;
    const auto& layout = *state.formatted;
    switch (slot) {
    case text_layout_GetLineMetrics: {
        const GuestAddr out = m.arg_ptr(1);
        const u32 max = static_cast<u32>(m.arg(2));
        const GuestAddr actual = m.arg_ptr(3);
        const u32 count = static_cast<u32>(layout.lines.size());
        // Probe: (NULL, 5) is E_INVALIDARG with actual 0; too small a
        // buffer reports E_NOT_SUFFICIENT_BUFFER and the real count.
        if (out == 0 && max != 0) {
            if (actual) (void)m_memory.write_u32(actual, 0);
            return E_INVALIDARG;
        }
        if (actual && m_memory.write_u32(actual, count).is_err()) return E_INVALIDARG;
        if (max < count) return E_NOT_SUFFICIENT_BUFFER;
        for (u32 i = 0; i < count; ++i) {
            const auto& line = layout.lines[i];
            if (!write_floats(out + i * size_LINE_METRICS[w],
                              {line.length, line.trailing_whitespace, line.newline, float_bits(line.height),
                               float_bits(line.baseline), line.trimmed ? 1u : 0u})) {
                return E_INVALIDARG;
            }
        }
        return S_OK;
    }
    case text_layout_GetMetrics: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_INVALIDARG;
        return write_floats(out, {float_bits(layout.left), float_bits(layout.top), float_bits(layout.width),
                                  float_bits(layout.width_with_whitespace), float_bits(layout.height),
                                  float_bits(state.max_width), float_bits(state.max_height),
                                  layout.max_bidi_depth, static_cast<u32>(layout.lines.size())})
                   ? S_OK : E_INVALIDARG;
    }
    case text_layout_GetOverhangMetrics: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_INVALIDARG;
        return write_floats(out, {float_bits(layout.overhang_left), float_bits(layout.overhang_top),
                                  float_bits(layout.overhang_right), float_bits(layout.overhang_bottom)})
                   ? S_OK : E_INVALIDARG;
    }
    case text_layout_GetClusterMetrics: {
        const GuestAddr out = m.arg_ptr(1);
        const u32 max = static_cast<u32>(m.arg(2));
        const GuestAddr actual = m.arg_ptr(3);
        const u32 count = static_cast<u32>(layout.clusters.size());
        if (actual && m_memory.write_u32(actual, count).is_err()) return E_INVALIDARG;
        if (max < count || (out == 0 && count != 0)) return E_NOT_SUFFICIENT_BUFFER;
        for (u32 i = 0; i < count; ++i) {
            const auto& c = layout.clusters[i];
            const GuestAddr at = out + i * size_CLUSTER_METRICS[w];
            const u16 flags = static_cast<u16>(
                (c.can_wrap_after ? 1u << bit_CLUSTER_METRICS_canWrapLineAfter : 0u) |
                (c.whitespace ? 1u << bit_CLUSTER_METRICS_isWhitespace : 0u) |
                (c.newline ? 1u << bit_CLUSTER_METRICS_isNewline : 0u) |
                (c.soft_hyphen ? 1u << bit_CLUSTER_METRICS_isSoftHyphen : 0u) |
                (c.right_to_left ? 1u << bit_CLUSTER_METRICS_isRightToLeft : 0u));
            if (m_memory.write_u32(at + offset_CLUSTER_METRICS_width[w], float_bits(c.width)).is_err() ||
                m_memory.write_u16(at + offset_CLUSTER_METRICS_length[w], static_cast<u16>(c.length)).is_err() ||
                m_memory.write_u16(at + offset_CLUSTER_METRICS_canWrapLineAfter[w], flags).is_err()) {
                return E_INVALIDARG;
            }
        }
        return S_OK;
    }
    case text_layout_DetermineMinWidth: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_INVALIDARG;
        return m_memory.write_u32(out, float_bits(layout.min_width)).is_ok() ? S_OK : E_INVALIDARG;
    }
    case text_layout_HitTestPoint:
    case text_layout_HitTestTextPosition:
    case text_layout_HitTestTextRange:
        return hit_test(m, state, slot);
    default:
        return E_NOTIMPL;
    }
}

namespace {

void write_hit(GuestMemory& memory, GuestAddr at, const Hit& h) {
    const u32 words[9]{h.position, h.length, float_bits(h.left), float_bits(h.top), float_bits(h.width),
                       float_bits(h.height), h.bidi, h.text ? 1u : 0u, h.trimmed ? 1u : 0u};
    (void)memory.write_bytes(at, reinterpret_cast<const Byte*>(words), sizeof(words));
}

/// The hit for a position at or past the end: the end of the text,
/// length 0 (probe).
Hit end_hit(const dwrite::TextLayoutResult& layout, u32 length) {
    Hit hit;
    hit.position = length;
    if (!layout.clusters.empty()) {
        const auto& last = layout.clusters.back();
        hit = cluster_hit(layout, last);
        hit.position = length;
        hit.length = 0;
        hit.left = last.right_to_left ? last.x : last.x + last.width;
        hit.width = 0;
    } else if (!layout.lines.empty()) {
        hit.left = layout.lines[0].left;
        hit.top = layout.lines[0].top;
        hit.height = layout.lines[0].height;
    }
    return hit;
}

}  // namespace

u32 DWriteManager::hit_test(Marshaler& m, TextState& state, u32 slot) {
    const auto& layout = *state.formatted;
    const u32 length = static_cast<u32>(state.text.size());
    if (slot == text_layout_HitTestTextPosition) {
        // (position, BOOL trailing, FLOAT* x, FLOAT* y, metrics*)
        const u32 position = static_cast<u32>(m.arg(1));
        const bool trailing = static_cast<u32>(m.arg(2)) != 0;
        const GuestAddr px = m.arg_ptr(3), py = m.arg_ptr(4), out = m.arg_ptr(5);
        if (px == 0 || py == 0 || out == 0) return E_INVALIDARG;
        Hit hit = end_hit(layout, length);
        float x = hit.left;
        for (const auto& c : layout.clusters) {
            if (position < c.text_start || position >= c.text_start + c.length) continue;
            hit = cluster_hit(layout, c);
            const bool far = trailing != c.right_to_left;
            x = far ? c.x + c.width : c.x;
            break;
        }
        write_hit(m_memory, out, hit);
        return m_memory.write_u32(px, float_bits(x)).is_ok() &&
                       m_memory.write_u32(py, float_bits(hit.top)).is_ok()
                   ? S_OK : E_INVALIDARG;
    }
    if (slot == text_layout_HitTestPoint) {
        // (FLOAT x, FLOAT y, BOOL* trailing, BOOL* inside, metrics*)
        const float x = read_float_argument(m, 1);
        const float y = read_float_argument(m, 2);
        const GuestAddr trailing_out = m.arg_ptr(3), inside_out = m.arg_ptr(4), out = m.arg_ptr(5);
        if (trailing_out == 0 || inside_out == 0 || out == 0) return E_INVALIDARG;
        bool inside = true, trailing = false;
        usize line_index = 0;
        if (layout.lines.empty()) return E_FAIL;
        if (y < layout.lines.front().top) inside = false;
        for (usize i = 0; i < layout.lines.size(); ++i) {
            line_index = i;
            if (y < layout.lines[i].top + layout.lines[i].height) break;
            if (i + 1 == layout.lines.size()) inside = false;
        }
        const dwrite::LayoutCluster* chosen = nullptr;
        const dwrite::LayoutCluster* first = nullptr;
        const dwrite::LayoutCluster* last = nullptr;
        for (const auto& c : layout.clusters) {
            if (c.line != line_index) continue;
            if (!first || c.x < first->x) first = &c;
            if (!last || c.x + c.width > last->x + last->width ||
                (c.newline && c.x + c.width >= last->x + last->width)) {
                last = &c;
            }
            if (x >= c.x && x < c.x + c.width) chosen = &c;
        }
        Hit hit;
        if (chosen) {
            hit = cluster_hit(layout, *chosen);
            trailing = (x >= chosen->x + chosen->width / 2) != chosen->right_to_left;
        } else if (first && x < first->x) {
            hit = cluster_hit(layout, *first);
            trailing = first->right_to_left;
            inside = false;
        } else if (last) {
            hit = cluster_hit(layout, *last);
            // Past a line that ends in a newline lands on it, leading edge.
            trailing = !last->newline;
            inside = false;
        } else {
            hit = end_hit(layout, length);
            inside = false;
        }
        write_hit(m_memory, out, hit);
        return m_memory.write_u32(trailing_out, trailing ? 1 : 0).is_ok() &&
                       m_memory.write_u32(inside_out, inside ? 1 : 0).is_ok()
                   ? S_OK : E_INVALIDARG;
    }
    // HitTestTextRange(position, length, FLOAT originX, FLOAT originY,
    //                  metrics*, UINT32 max, UINT32* actual)
    const u32 position = static_cast<u32>(m.arg(1));
    const u32 range = static_cast<u32>(m.arg(2));
    const float ox = read_float_argument(m, 3);
    const float oy = read_float_argument(m, 4);
    const GuestAddr out = m.arg_ptr(5);
    const u32 max = static_cast<u32>(m.arg(6));
    const GuestAddr actual = m.arg_ptr(7);
    if (actual == 0) return E_INVALIDARG;
    const u64 end = std::min<u64>(u64{position} + range, length);
    std::vector<Hit> hits;
    for (const auto& c : layout.clusters) {
        if (c.text_start + c.length <= position || c.text_start >= end) continue;
        const Hit h = cluster_hit(layout, c);
        if (!hits.empty() && layout.clusters[0].line <= c.line &&
            hits.back().top == h.top && std::abs(hits.back().left + hits.back().width - h.left) < 0.01f) {
            hits.back().width += h.width;
            hits.back().length = c.text_start + c.length - hits.back().position;
            continue;
        }
        hits.push_back(h);
    }
    if (hits.empty()) hits.push_back(end_hit(layout, std::min<u32>(position, length)));
    for (auto& h : hits) {
        h.left += ox;
        h.top += oy;
    }
    if (m_memory.write_u32(actual, static_cast<u32>(hits.size())).is_err()) return E_INVALIDARG;
    if (max < hits.size() || out == 0) return E_NOT_SUFFICIENT_BUFFER;
    for (usize i = 0; i < hits.size(); ++i) write_hit(m_memory, out + i * 36, hits[i]);
    return S_OK;
}

u32 DWriteManager::render_to(TextState& state, GuestAddr context, GuestAddr renderer, float ox, float oy) {
    const auto& layout = *state.formatted;
    const usize w = m_is_64bit ? 1 : 0;
    if (!ensure_storage()) return E_OUTOFMEMORY;
    // Probe callback order: IsPixelSnappingDisabled, GetPixelsPerDip,
    // GetCurrentTransform; a failure from any of them aborts Draw.
    const GuestAddr scratch = m_callback_scratch;
    const auto check = [](const std::optional<u64>& hr) {
        return hr ? static_cast<u32>(*hr) : E_FAIL;
    };
    (void)m_memory.write_u32(scratch, 0);
    const u64 snap_args[]{context, scratch};
    if (const u32 hr = check(call_guest_method(renderer, 3, snap_args, 2)); hr & 0x80000000u) return hr;
    const bool snapping = m_memory.read_u32(scratch).value_or(0) == 0;
    const u64 ppd_args[]{context, scratch + 4};
    (void)m_memory.write_u32(scratch + 4, float_bits(1));
    if (const u32 hr = check(call_guest_method(renderer, 5, ppd_args, 2)); hr & 0x80000000u) return hr;
    const float ppd = float_from_bits(m_memory.read_u32(scratch + 4).value_or(float_bits(1)));
    const u32 identity[6]{float_bits(1), 0, 0, float_bits(1), 0, 0};
    (void)m_memory.write_bytes(scratch + 8, reinterpret_cast<const Byte*>(identity), sizeof(identity));
    const u64 transform_args[]{context, scratch + 8};
    if (const u32 hr = check(call_guest_method(renderer, 4, transform_args, 2)); hr & 0x80000000u) return hr;
    std::array<float, 6> t{};
    (void)m_memory.read_bytes(scratch + 8, reinterpret_cast<Byte*>(t.data()), sizeof(t));
    // Probe: with snapping on, glyph-run and underline baselines land on
    // whole device pixels (x is not snapped); strikethroughs are not snapped.
    const auto snap = [&](float x, float y) {
        if (!snapping || t[1] != 0 || t[2] != 0 || t[3] == 0 || !(ppd > 0)) return y;
        const float device = (t[3] * y + t[1] * x + t[5]) * ppd;
        return (std::round(device) / ppd - t[5] - t[1] * x) / t[3];
    };

    usize bytes = 4096;
    for (const auto& run : layout.runs) bytes += run.glyphs.size() * 16 + run.text_length * 4 + 256;
    DrawScratch memory(m_memory, bytes + state.text.size() * 2);
    if (memory.base == 0) return E_OUTOFMEMORY;
    const GuestAddr text = memory.take(state.text.size() * 2 + 2);
    (void)m_memory.write_bytes(text, reinterpret_cast<const Byte*>(state.text.data()), state.text.size() * 2);

    std::vector<std::pair<std::pair<std::u16string, u32>, GuestAddr>> faces;
    const auto face_for = [&](const dwrite::MatchedFont& font, u32 simulations) -> GuestAddr {
        const std::pair<std::u16string, u32> key{font.postscript_name + u"/" + font.family_name, simulations};
        for (const auto& [k, face] : faces) if (k == key) return face;
        const GuestAddr face = create_object(ObjectType::FontFace, ensure_family_index(font.family_name), font);
        if (face == 0) return 0;
        m_objects.at(face).simulations = simulations;
        faces.push_back({key, face});
        return face;
    };
    struct FaceRelease {
        DWriteManager& manager;
        std::vector<std::pair<std::pair<std::u16string, u32>, GuestAddr>>& faces;
        ~FaceRelease() { for (const auto& [k, face] : faces) manager.release_com(face); }
    } release_faces{*this, faces};

    const auto write_string = [&](const std::u16string& value) -> GuestAddr {
        const GuestAddr at = memory.take(value.size() * 2 + 2);
        if (at == 0) return 0;
        (void)m_memory.write_bytes(at, reinterpret_cast<const Byte*>(value.data()), value.size() * 2);
        (void)m_memory.write_u16(at + value.size() * 2, 0);
        return at;
    };
    const auto decorations_list = decorations(state, layout);
    for (usize line = 0; line < layout.lines.size(); ++line) {
        for (const auto& run : layout.runs) {
            if (run.line != line) continue;
            const auto& spec = state.spec_at(run.text_start);
            if (run.inline_object) {
                const GuestAddr object = run.trimming_sign ? state.trimming_sign : spec.inline_object;
                const float left = (run.bidi_level & 1) ? run.baseline_x - run.width : run.baseline_x;
                const u64 args[]{context, float_bits(ox + left), float_bits(oy + run.baseline_y - run.inline_baseline),
                                 object, 0, (run.bidi_level & 1) ? 1u : 0u,
                                 run.trimming_sign ? state.spec_at(run.text_start).effect : spec.effect};
                (void)call_guest_method(renderer, 9, args, std::size(args));  // DrawInlineObject
                continue;
            }
            const GuestAddr face = face_for(run.font, run.simulations);
            const usize count = run.glyphs.size();
            const GuestAddr glyphs = memory.take(std::max<usize>(count, 1) * 2);
            const GuestAddr advances = memory.take(std::max<usize>(count, 1) * 4);
            const GuestAddr offsets = memory.take(std::max<usize>(count, 1) * 8);
            const GuestAddr clusters = memory.take(std::max<usize>(run.text_length, 1) * 2);
            const GuestAddr glyph_run = memory.take(size_GLYPH_RUN[w]);
            const GuestAddr description = memory.take(size_GLYPH_RUN_DESCRIPTION[w]);
            const GuestAddr locale = write_string(spec.locale);
            if (face == 0 || !glyphs || !advances || !offsets || !clusters || !glyph_run || !description || !locale) {
                return E_OUTOFMEMORY;
            }
            for (usize g = 0; g < count; ++g) {
                (void)m_memory.write_u16(glyphs + g * 2, run.glyphs[g]);
                (void)m_memory.write_u32(advances + g * 4, float_bits(run.advances[g]));
                (void)m_memory.write_u32(offsets + g * 8, float_bits(run.offsets[g].advance_offset));
                (void)m_memory.write_u32(offsets + g * 8 + 4, float_bits(run.offsets[g].ascender_offset));
            }
            for (usize u = 0; u < run.text_length; ++u) (void)m_memory.write_u16(clusters + u * 2, run.cluster_map[u]);
            (void)write_pointer(glyph_run + offset_GLYPH_RUN_fontFace[w], face);
            (void)m_memory.write_u32(glyph_run + offset_GLYPH_RUN_fontEmSize[w], float_bits(run.size));
            (void)m_memory.write_u32(glyph_run + offset_GLYPH_RUN_glyphCount[w], static_cast<u32>(count));
            (void)write_pointer(glyph_run + offset_GLYPH_RUN_glyphIndices[w], glyphs);
            (void)write_pointer(glyph_run + offset_GLYPH_RUN_glyphAdvances[w], advances);
            (void)write_pointer(glyph_run + offset_GLYPH_RUN_glyphOffsets[w], offsets);
            (void)m_memory.write_u32(glyph_run + offset_GLYPH_RUN_isSideways[w], 0);
            (void)m_memory.write_u32(glyph_run + offset_GLYPH_RUN_bidiLevel[w], run.bidi_level);
            (void)write_pointer(description + offset_GLYPH_RUN_DESCRIPTION_localeName[w], locale);
            (void)write_pointer(description + offset_GLYPH_RUN_DESCRIPTION_string[w], text + run.text_start * 2);
            (void)m_memory.write_u32(description + offset_GLYPH_RUN_DESCRIPTION_stringLength[w], run.text_length);
            (void)write_pointer(description + offset_GLYPH_RUN_DESCRIPTION_clusterMap[w], clusters);
            (void)m_memory.write_u32(description + offset_GLYPH_RUN_DESCRIPTION_textPosition[w], run.text_start);
            const float x = ox + run.baseline_x;
            const u64 args[]{context, float_bits(x), float_bits(snap(x, oy + run.baseline_y)),
                             0 /* DWRITE_MEASURING_MODE_NATURAL */, glyph_run, description, spec.effect};
            // Probe: a failing DrawGlyphRun does not stop Draw.
            (void)call_guest_method(renderer, 6, args, std::size(args));
        }
        for (const auto& d : decorations_list) {
            const auto& run = layout.runs[d.first_run];
            if (run.line != line) continue;
            const GuestAddr record = memory.take(d.strikethrough ? size_STRIKETHROUGH[w] : size_UNDERLINE[w]);
            const GuestAddr locale = d.strikethrough ? 0 : write_string(d.locale);
            if (!record) return E_OUTOFMEMORY;
            const float x = ox + d.x;
            float y = oy + run.baseline_y;
            if (d.strikethrough) {
                (void)write_floats_at(record, {float_bits(d.width), float_bits(d.thickness), float_bits(d.offset),
                                               state.reading_direction, state.flow_direction});
                (void)write_pointer(record + offset_STRIKETHROUGH_localeName[w], 0);
                (void)m_memory.write_u32(record + offset_STRIKETHROUGH_measuringMode[w], 0);
            } else {
                y = snap(x, y);
                (void)write_floats_at(record, {float_bits(d.width), float_bits(d.thickness), float_bits(d.offset),
                                               float_bits(d.run_height), state.reading_direction,
                                               state.flow_direction});
                (void)write_pointer(record + offset_UNDERLINE_localeName[w], locale);
                (void)m_memory.write_u32(record + offset_UNDERLINE_measuringMode[w], 0);
            }
            const u64 args[]{context, float_bits(x), float_bits(y), record, d.effect};
            (void)call_guest_method(renderer, d.strikethrough ? 8 : 7, args, std::size(args));
        }
    }
    return S_OK;
}

bool DWriteManager::write_floats_at(GuestAddr at, std::initializer_list<u32> words) {
    usize i = 0;
    for (const u32 word : words) {
        if (m_memory.write_u32(at + 4 * i++, word).is_err()) return false;
    }
    return true;
}

void DWriteManager::trimming_sign_method(Marshaler& m, GuestAddr self, TextState& state, u32 slot) {
    (void)self;
    TextState& sign = *state.sign_format;
    switch (slot) {
    case inline_object_Draw: {
        // (context, renderer, FLOAT originX, FLOAT originY, BOOL sideways,
        //  BOOL rightToLeft, IUnknown* effect): the ellipsis as a glyph run
        // whose box top-left is the origin.
        const GuestAddr context = m.arg_ptr(1);
        const GuestAddr renderer = m.arg_ptr(2);
        const float x = read_float_argument(m, 3);
        const float y = read_float_argument(m, 4);
        if (renderer == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (const u32 hr = format_text(sign); hr != S_OK) {
            m.set_return(hr);
            return;
        }
        const GuestAddr effect = m.arg_ptr(6);
        if (effect != 0 && !sign.runs.empty()) sign.runs.front().spec.effect = effect;
        const u32 hr = render_to(sign, context, renderer, x, y);
        if (!sign.runs.empty()) sign.runs.front().spec.effect = 0;
        m.set_return(hr);
        return;
    }
    case inline_object_GetMetrics: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (const u32 hr = format_text(sign); hr != S_OK) {
            m.set_return(hr);
            return;
        }
        const auto& layout = *sign.formatted;
        m.set_return(write_floats_at(out, {float_bits(layout.width), float_bits(layout.lines[0].height),
                                           float_bits(layout.lines[0].baseline), 0})
                         ? S_OK : E_INVALIDARG);
        return;
    }
    case inline_object_GetOverhangMetrics: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (const u32 hr = format_text(sign); hr != S_OK) {
            m.set_return(hr);
            return;
        }
        // Relative to the sign's own box (its width x line height).
        const auto& layout = *sign.formatted;
        const float right = layout.overhang_right + sign.max_width - layout.width;
        const float bottom = layout.overhang_bottom + sign.max_height - layout.lines[0].height;
        m.set_return(write_floats_at(out, {float_bits(layout.overhang_left), float_bits(layout.overhang_top),
                                           float_bits(std::isfinite(right) ? right : 0),
                                           float_bits(std::isfinite(bottom) ? bottom : 0)})
                         ? S_OK : E_INVALIDARG);
        return;
    }
    case inline_object_GetBreakConditions: {
        const GuestAddr before = m.arg_ptr(1), after = m.arg_ptr(2);
        if (before == 0 || after == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        (void)m_memory.write_u32(before, DWRITE_BREAK_CONDITION_NEUTRAL);
        (void)m_memory.write_u32(after, DWRITE_BREAK_CONDITION_NEUTRAL);
        m.set_return(S_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

u32 DWriteManager::text_drawing(GuestAddr object, TextDrawing& out) {
    std::lock_guard lock(m_api_mutex);
    const auto found = m_objects.find(object);
    if (found == m_objects.end() || found->second.type != ObjectType::TextLayout || !found->second.text) {
        return E_INVALIDARG;
    }
    return collect_drawing(*found->second.text, out);
}

u32 DWriteManager::text_drawing_for_format(GuestAddr format, std::u16string text, float width,
                                           float height, TextDrawing& out) {
    std::lock_guard lock(m_api_mutex);
    const auto found = m_objects.find(format);
    if (found == m_objects.end() || !found->second.text ||
        (found->second.type != ObjectType::TextFormat && found->second.type != ObjectType::TextLayout)) {
        return E_INVALIDARG;
    }
    // A transient layout: the format keeps every object it references alive.
    TextState layout = *found->second.text;
    layout.text = std::move(text);
    layout.max_width = width;
    layout.max_height = height;
    layout.runs = {{0, layout.format}};
    layout.invalidate();
    return collect_drawing(layout, out);
}

u32 DWriteManager::collect_drawing(TextState& state, TextDrawing& out) {
    if (const u32 hr = format_text(state); hr != S_OK) return hr;
    out = {};
    out.layout = *state.formatted;
    out.max_width = state.max_width;
    out.max_height = state.max_height;
    std::vector<dwrite::LayoutGlyphRun> runs;
    std::vector<GuestAddr> effects;
    for (const auto& run : out.layout.runs) {
        const auto& spec = state.spec_at(run.text_start);
        if (run.trimming_sign) {
            const auto sign = m_objects.find(state.trimming_sign);
            if (sign != m_objects.end() && sign->second.type == ObjectType::TrimmingSign) {
                // Our ellipsis: its glyphs, placed on this line's baseline.
                TextState& sign_state = *sign->second.text->sign_format;
                if (format_text(sign_state) != S_OK) continue;
                const auto& sign_layout = *sign_state.formatted;
                for (auto glyphs : sign_layout.runs) {
                    if (glyphs.glyphs.empty()) continue;
                    glyphs.baseline_x += run.baseline_x;
                    glyphs.baseline_y = run.baseline_y;
                    glyphs.line = run.line;
                    glyphs.text_start = run.text_start;
                    glyphs.text_length = 0;
                    glyphs.cluster_map.clear();
                    runs.push_back(std::move(glyphs));
                    effects.push_back(spec.effect);
                }
            } else {
                out.inline_objects.push_back({state.trimming_sign, run.baseline_x,
                                              run.baseline_y - run.inline_baseline, spec.effect,
                                              (run.bidi_level & 1) != 0});
            }
            continue;
        }
        if (run.inline_object) {
            const float left = (run.bidi_level & 1) ? run.baseline_x - run.width : run.baseline_x;
            out.inline_objects.push_back({spec.inline_object, left, run.baseline_y - run.inline_baseline,
                                          spec.effect, (run.bidi_level & 1) != 0});
            continue;
        }
        runs.push_back(run);
        effects.push_back(spec.effect);
    }
    for (const auto& d : decorations(state, *state.formatted)) {
        const auto& run = state.formatted->runs[d.first_run];
        out.decorations.push_back({d.x, run.baseline_y, d.width, d.thickness, d.offset, d.effect, d.strikethrough});
    }
    out.layout.runs = std::move(runs);
    out.run_effects = std::move(effects);
    return S_OK;
}


}  // namespace vitreo::winapi

namespace vitreo::winapi {

bool DWriteManager::font_face_description(GuestAddr face, dwrite::MatchedFont& font, u32& simulations) {
    std::lock_guard lock(m_api_mutex);
    const auto found = m_objects.find(face);
    if (found == m_objects.end() || found->second.type != ObjectType::FontFace) return false;
    font = found->second.font;
    simulations = found->second.simulations;
    return true;
}

std::optional<u32> DWriteManager::rendering_params_mode(GuestAddr params) {
    std::lock_guard lock(m_api_mutex);
    const auto found = m_objects.find(params);
    if (found == m_objects.end() || found->second.type != ObjectType::RenderingParams) return std::nullopt;
    return found->second.rendering_mode;
}

}  // namespace vitreo::winapi
