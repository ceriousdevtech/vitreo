// SPDX-License-Identifier: LGPL-2.1-only
// IDWriteFactory::CreateTextFormat / CreateTextLayout /
// CreateGdiCompatibleTextLayout / CreateEllipsisTrimmingSign and the
// IDWriteTextFormat + IDWriteTextLayout property surface (dwrite.h). The
// formatting itself lives in dwrite_text_layout.cpp and text_layout_engine.
// Argument rules not stated on Microsoft Learn come from
// tools/oracle_probe/dwrite_text_layout_probe.
#include "winapi/graphics/dwrite/dwrite_text_internal.hpp"

#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace vitreo::winapi {
using namespace dwrite_text_contract;
namespace {

struct Range {
    u32 start{0};
    u32 length{0};
};

// DWRITE_TEXT_RANGE is passed by value: two stack dwords on x86, one
// register/stack slot on x64.
Range read_range(Marshaler& m, usize index) {
    if (m.cpu_is_64bit()) {
        const u64 packed = m.arg(index);
        return {static_cast<u32>(packed), static_cast<u32>(packed >> 32)};
    }
    return {static_cast<u32>(m.arg(index)), static_cast<u32>(m.arg(index + 1))};
}

bool valid_weight(u32 weight) { return weight >= 1 && weight <= 999; }
bool valid_style(u32 style) { return style <= DWRITE_FONT_STYLE_ITALIC; }
bool valid_stretch(u32 stretch) {
    return stretch >= DWRITE_FONT_STRETCH_ULTRA_CONDENSED &&
           stretch <= DWRITE_FONT_STRETCH_ULTRA_EXPANDED;
}
// dwrite_text_layout_probe: CreateTextFormat accepts fontSize=INFINITY and
// rejects 0, negative and NaN.
bool valid_size(float size) { return !std::isnan(size) && size > 0; }

}  // namespace

GuestAddr DWriteManager::create_text_object(ObjectType type, std::shared_ptr<TextState> state) {
    const GuestAddr object = create_object(type);
    if (object == 0) return 0;
    m_objects.at(object).text = std::move(state);
    return object;
}

void DWriteManager::retain_com(GuestAddr object) {
    if (object == 0) return;
    if (auto ours = m_objects.find(object); ours != m_objects.end()) {
        ++ours->second.ref_count;
        return;
    }
    (void)call_guest_method(object, 1, nullptr, 0);
}

void DWriteManager::release_com(GuestAddr object) {
    if (object == 0) return;
    if (auto ours = m_objects.find(object); ours != m_objects.end()) {
        if (ours->second.ref_count > 1) {
            --ours->second.ref_count;
            return;
        }
        // Last reference: run the same teardown a guest Release would.
        (void)call_guest_method(object, 2, nullptr, 0);
        return;
    }
    release_guest_object(object);
}

namespace {
std::set<GuestAddr> held_references(const DWriteManager::TextState& state);
}

void DWriteManager::release_text_state(TextState& state) {
    for (const GuestAddr object : held_references(state)) release_com(object);
    state.runs.clear();
    state.format = {};
    state.trimming_sign = 0;
}

namespace {

std::set<GuestAddr> held_references(const DWriteManager::TextState& state) {
    std::set<GuestAddr> held;
    const auto add_spec = [&](const DWriteManager::TextStyleSpec& spec) {
        for (const GuestAddr object : {spec.collection, spec.effect, spec.inline_object,
                                       spec.typography}) {
            if (object != 0) held.insert(object);
        }
    };
    add_spec(state.format);
    for (const auto& run : state.runs) add_spec(run.spec);
    if (state.trimming_sign != 0) held.insert(state.trimming_sign);
    return held;
}

}  // namespace

std::optional<dwrite::MatchedFont> DWriteManager::resolve_text_font(
    GuestAddr collection, const std::u16string& family, u32 weight, u32 style,
    u32 stretch, u32& simulations) {
    simulations = 0;
    std::shared_ptr<std::vector<dwrite::MatchedFont>> fonts;
    const auto object = m_objects.find(collection);
    const std::u16string wanted = fold_ascii(family);
    if (object != m_objects.end() && object->second.custom_collection) {
        for (const auto& candidate : object->second.collection_families) {
            if (candidate && !candidate->empty() &&
                fold_ascii(candidate->front().family_name) == wanted) {
                fonts = candidate;
                break;
            }
        }
    } else {
        const auto found = std::find_if(
            m_font_families.begin(), m_font_families.end(),
            [&](const std::u16string& name) { return fold_ascii(name) == wanted; });
        if (found != m_font_families.end()) {
            Object probe{};
            probe.type = ObjectType::FontFamily;
            probe.family_index = static_cast<usize>(std::distance(m_font_families.begin(), found));
            fonts = ensure_family_fonts(probe);
            if (!fonts || fonts->empty()) {
                // A listed family with no enumerable faces (the Windows UI
                // family on a host without it) resolves as the family lookup
                // does elsewhere: the host UI face under the requested name.
                auto matched = dwrite::match_system_font(family, weight, stretch, style);
                if (!matched) return std::nullopt;
                if (weight >= 600 && matched->weight <= 500) simulations |= DWRITE_FONT_SIMULATIONS_BOLD;
                if (style != 0 && matched->style == 0) simulations |= DWRITE_FONT_SIMULATIONS_OBLIQUE;
                return matched;
            }
        }
    }
    if (!fonts || fonts->empty()) return std::nullopt;
    // IDWriteFontFamily::GetFirstMatchingFont's ordering (dwrite_collection.cpp).
    const auto score = [&](const dwrite::MatchedFont& font) {
        const u32 weight_distance = font.weight > weight ? font.weight - weight : weight - font.weight;
        const u32 stretch_distance = font.stretch > stretch ? font.stretch - stretch : stretch - font.stretch;
        const u32 style_distance = font.style == style ? 0u : 1u;
        return weight_distance + stretch_distance * 1000u + style_distance * 10000u;
    };
    const auto best = *std::min_element(fonts->begin(), fonts->end(),
                                        [&](const auto& l, const auto& r) { return score(l) < score(r); });
    // Learn DWRITE_FONT_SIMULATIONS: a bold request on a face that is not
    // (weight 600+ requested, face at most 500) is emboldened; an italic or
    // oblique request on an upright face is slanted.
    if (weight >= 600 && best.weight <= 500) simulations |= DWRITE_FONT_SIMULATIONS_BOLD;
    if (style != 0 && best.style == 0) simulations |= DWRITE_FONT_SIMULATIONS_OBLIQUE;
    return best;
}

void DWriteManager::impl_CreateTextFormat(Marshaler& m) {
    // (this, family, collection, weight, style, stretch, FLOAT size, locale, format**)
    const GuestAddr factory = m.arg_ptr(0);
    const GuestAddr family_address = m.arg_ptr(1);
    const GuestAddr collection = m.arg_ptr(2);
    const u32 weight = static_cast<u32>(m.arg(3));
    const u32 style = static_cast<u32>(m.arg(4));
    const u32 stretch = static_cast<u32>(m.arg(5));
    const float size = read_float_argument(m, 6);
    const GuestAddr locale_address = m.arg_ptr(7);
    const GuestAddr out = m.arg_ptr(8);
    // dwrite_text_layout_probe: a NULL out pointer is not checked (access
    // violation); every E_INVALIDARG first clears it.
    if (m.write_ptr(out, 0).is_err()) {
        (void)m.raise_access_violation(true, out);
        return;
    }
    const auto owner = m_objects.find(factory);
    const auto chosen = m_objects.find(collection);
    if (owner == m_objects.end() || owner->second.type != ObjectType::Factory ||
        family_address == 0 || locale_address == 0 || !valid_weight(weight) ||
        !valid_style(style) || !valid_stretch(stretch) || !valid_size(size) ||
        (collection != 0 && (chosen == m_objects.end() ||
                             chosen->second.type != ObjectType::FontCollection))) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto family = read_utf16_z(family_address, 32768);
    auto locale = read_utf16_z(locale_address, 85);
    if (family.is_err() || locale.is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto state = std::make_shared<TextState>();
    state->format.family = std::move(family.value());
    state->format.locale = std::move(locale.value());
    state->format.weight = weight;
    state->format.style = style;
    state->format.stretch = stretch;
    state->format.size = size;
    GuestAddr held_collection = collection;
    if (held_collection == 0) {
        // The system collection, as GetSystemFontCollection hands it out.
        if (m_system_collection == 0) {
            m_system_collection = create_object(ObjectType::FontCollection);
            held_collection = m_system_collection;
        } else {
            held_collection = m_system_collection;
            retain_com(held_collection);
        }
        if (held_collection == 0) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
    } else {
        retain_com(held_collection);
    }
    state->format.collection = held_collection;
    const GuestAddr object = create_text_object(ObjectType::TextFormat, state);
    if (object == 0 || m.write_ptr(out, object).is_err()) {
        if (object != 0) {
            m_objects.erase(object);
            m_free_objects.push_back(object);
        }
        release_text_state(*state);
        m.set_return(object == 0 ? E_OUTOFMEMORY : E_INVALIDARG);
        return;
    }
    m.set_return(S_OK);
}

namespace {

/// The IDWriteTextFormat half of a format or layout object, copied into a
/// new layout (CreateTextLayout reads the format's current properties).
void copy_format(const DWriteManager::TextState& from, DWriteManager::TextState& to) {
    to.format = from.format;
    to.text_alignment = from.text_alignment;
    to.paragraph_alignment = from.paragraph_alignment;
    to.word_wrapping = from.word_wrapping;
    to.reading_direction = from.reading_direction;
    to.flow_direction = from.flow_direction;
    to.incremental_tab_stop = from.incremental_tab_stop;
    to.trimming_granularity = from.trimming_granularity;
    to.trimming_delimiter = from.trimming_delimiter;
    to.trimming_delimiter_count = from.trimming_delimiter_count;
    to.trimming_sign = from.trimming_sign;
    to.line_spacing_method = from.line_spacing_method;
    to.line_spacing = from.line_spacing;
    to.baseline = from.baseline;
}

}  // namespace

namespace {
u32 create_layout(DWriteManager& manager, Marshaler& m, bool gdi);
}

void DWriteManager::impl_CreateTextLayout(Marshaler& m) {
    // (this, string, length, format, FLOAT maxWidth, FLOAT maxHeight, layout**)
    const GuestAddr out = m.arg_ptr(6);
    if (m.write_ptr(out, 0).is_err()) {
        (void)m.raise_access_violation(true, out);
        return;
    }
    const GuestAddr factory = m.arg_ptr(0);
    const GuestAddr string = m.arg_ptr(1);
    const u32 length = static_cast<u32>(m.arg(2));
    const auto format = m_objects.find(m.arg_ptr(3));
    const float width = read_float_argument(m, 4);
    const float height = read_float_argument(m, 5);
    const auto owner = m_objects.find(factory);
    if (owner == m_objects.end() || owner->second.type != ObjectType::Factory ||
        format == m_objects.end() || !format->second.text ||
        (format->second.type != ObjectType::TextFormat &&
         format->second.type != ObjectType::TextLayout) ||
        string == 0 || std::isnan(width) || std::isnan(height) ||
        width < 0 || height < 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto text = length == 0 ? VResult<std::u16string>::ok({}) : read_utf16(string, length);
    if (text.is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto state = std::make_shared<TextState>();
    copy_format(*format->second.text, *state);
    state->text = std::move(text.value());
    state->max_width = width;
    state->max_height = height;
    state->runs.push_back({0, state->format});
    for (const GuestAddr object : held_references(*state)) retain_com(object);
    const GuestAddr object = create_text_object(ObjectType::TextLayout, state);
    if (object == 0 || m.write_ptr(out, object).is_err()) {
        if (object != 0) {
            m_objects.erase(object);
            m_free_objects.push_back(object);
        }
        release_text_state(*state);
        m.set_return(object == 0 ? E_OUTOFMEMORY : E_INVALIDARG);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_CreateGdiCompatibleTextLayout(Marshaler& m) {
    // (this, string, length, format, FLOAT width, FLOAT height,
    //  FLOAT pixelsPerDip, DWRITE_MATRIX const* transform, BOOL natural, layout**)
    const GuestAddr out = m.arg_ptr(9);
    if (out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(out, 0);
    const float pixels_per_dip = read_float_argument(m, 6);
    const GuestAddr transform = m.arg_ptr(7);
    const bool natural = static_cast<u32>(m.arg(8)) != 0;
    std::array<float, 6> matrix{1, 0, 0, 1, 0, 0};
    if (transform != 0 && m_memory.read_bytes(transform, reinterpret_cast<Byte*>(matrix.data()), sizeof(matrix)).is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (!valid_size(pixels_per_dip) ||
        std::any_of(matrix.begin(), matrix.end(), [](float v) { return !std::isfinite(v); })) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // GDI-compatible measuring rounds advances to whole device pixels at
    // pixelsPerDip under the transform; the formatter only implements the
    // natural (ideal) model. Fail at the call site, not with ideal metrics.
    static std::once_flag once;
    std::call_once(once, [] {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED dwrite.dll!IDWriteFactory::"
                             "CreateGdiCompatibleTextLayout: GDI-classic/natural measuring\n");
    });
    (void)natural;
    m.set_return(E_NOTIMPL);
}

void DWriteManager::impl_CreateEllipsisTrimmingSign(Marshaler& m) {
    // (this, IDWriteTextFormat* format, IDWriteInlineObject** sign)
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(out, 0);
    const auto format = m_objects.find(m.arg_ptr(1));
    if (format == m_objects.end() || !format->second.text ||
        (format->second.type != ObjectType::TextFormat &&
         format->second.type != ObjectType::TextLayout)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto sign = std::make_shared<TextState>();
    sign->sign_format = std::make_shared<TextState>();
    copy_format(*format->second.text, *sign->sign_format);
    sign->sign_format->trimming_sign = 0;
    sign->sign_format->trimming_granularity = 0;
    sign->sign_format->text = u"…";
    sign->sign_format->max_width = std::numeric_limits<float>::infinity();
    sign->sign_format->max_height = std::numeric_limits<float>::infinity();
    sign->sign_format->word_wrapping = DWRITE_WORD_WRAPPING_NO_WRAP;
    sign->sign_format->runs.push_back({0, sign->sign_format->format});
    for (const GuestAddr object : held_references(*sign->sign_format)) retain_com(object);
    const GuestAddr object = create_text_object(ObjectType::TrimmingSign, sign);
    if (object == 0 || m.write_ptr(out, object).is_err()) {
        if (object != 0) {
            m_objects.erase(object);
            m_free_objects.push_back(object);
        }
        release_text_state(*sign->sign_format);
        m.set_return(object == 0 ? E_OUTOFMEMORY : E_INVALIDARG);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::text_method(Marshaler& m, TextInterface kind, u32 slot) {
    const GuestAddr self = m.arg_ptr(0);
    const auto object = m_objects.find(self);
    const ObjectType expected = kind == TextInterface::Format ? ObjectType::TextFormat
        : kind == TextInterface::Layout ? ObjectType::TextLayout : ObjectType::TrimmingSign;
    if (object == m_objects.end() || object->second.type != expected || !object->second.text) {
        (void)m.raise_access_violation(false, self);
        return;
    }
    // Keep the state alive across guest callbacks that may Release `self`.
    const auto state = object->second.text;
    if (kind == TextInterface::InlineObject) {
        trimming_sign_method(m, self, *state, slot);
    } else if (slot < text_layout_SetMaxWidth) {
        text_format_method(m, self, *state, slot);
    } else {
        text_layout_method(m, self, *state, slot);
    }
}

namespace {

/// Writes @p value NUL-terminated into a WCHAR[size] buffer.
/// dwrite_text_layout_probe: a short buffer still receives the truncated,
/// NUL-terminated prefix (size 5 -> "Aria"), with E_NOT_SUFFICIENT_BUFFER;
/// size 0 leaves it untouched.
u32 write_name(Marshaler& m, GuestAddr buffer, u32 size, const std::u16string& value) {
    if (buffer == 0) return E_INVALIDARG;
    if (size == 0) return E_NOT_SUFFICIENT_BUFFER;
    const usize copied = std::min<usize>(value.size(), size - 1);
    for (usize i = 0; i < copied; ++i) {
        if (m.write_u16(buffer + i * 2, value[i]).is_err()) return E_INVALIDARG;
    }
    if (m.write_u16(buffer + copied * 2, 0).is_err()) return E_INVALIDARG;
    return copied == value.size() ? S_OK : E_NOT_SUFFICIENT_BUFFER;
}

}  // namespace

void DWriteManager::text_format_method(Marshaler& m, GuestAddr self, TextState& state, u32 slot) {
    (void)self;
    const auto set_enum = [&](u32& field, u32 maximum) {
        const u32 value = static_cast<u32>(m.arg(1));
        if (value > maximum) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (field != value) state.invalidate();
        field = value;
        m.set_return(S_OK);
    };
    switch (slot) {
    case text_format_SetTextAlignment:
        set_enum(state.text_alignment, DWRITE_TEXT_ALIGNMENT_JUSTIFIED);
        return;
    case text_format_SetParagraphAlignment:
        set_enum(state.paragraph_alignment, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return;
    case text_format_SetWordWrapping:
        set_enum(state.word_wrapping, DWRITE_WORD_WRAPPING_CHARACTER);
        return;
    case text_format_SetReadingDirection:
        set_enum(state.reading_direction, DWRITE_READING_DIRECTION_BOTTOM_TO_TOP);
        return;
    case text_format_SetFlowDirection:
        set_enum(state.flow_direction, DWRITE_FLOW_DIRECTION_RIGHT_TO_LEFT);
        return;
    case text_format_SetIncrementalTabStop: {
        const float value = read_float_argument(m, 1);
        if (!valid_size(value)) {
            m.set_return(E_INVALIDARG);
            return;
        }
        state.incremental_tab_stop = value;
        state.invalidate();
        m.set_return(S_OK);
        return;
    }
    case text_format_SetTrimming: {
        const GuestAddr options = m.arg_ptr(1);
        const GuestAddr sign = m.arg_ptr(2);
        std::array<u32, 3> trimming{};
        if (options == 0 || m_memory.read_bytes(options, reinterpret_cast<Byte*>(trimming.data()), sizeof(trimming)).is_err() ||
            trimming[0] > DWRITE_TRIMMING_GRANULARITY_WORD) {
            m.set_return(E_INVALIDARG);
            return;
        }
        retain_com(sign);
        release_com(state.trimming_sign);
        state.trimming_sign = sign;
        state.trimming_granularity = trimming[0];
        state.trimming_delimiter = trimming[1];
        state.trimming_delimiter_count = trimming[2];
        state.invalidate();
        m.set_return(S_OK);
        return;
    }
    case text_format_SetLineSpacing: {
        const u32 method = static_cast<u32>(m.arg(1));
        const float spacing = read_float_argument(m, 2);
        const float baseline = read_float_argument(m, 3);
        // dwrite_text_layout_probe: negative or NaN spacing and an unknown
        // method fail; a negative baseline is accepted.
        if (method > DWRITE_LINE_SPACING_METHOD_PROPORTIONAL || std::isnan(spacing) ||
            std::isnan(baseline) || spacing < 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        state.line_spacing_method = method;
        state.line_spacing = spacing;
        state.baseline = baseline;
        state.invalidate();
        m.set_return(S_OK);
        return;
    }
    case text_format_GetTextAlignment: m.set_return(state.text_alignment); return;
    case text_format_GetParagraphAlignment: m.set_return(state.paragraph_alignment); return;
    case text_format_GetWordWrapping: m.set_return(state.word_wrapping); return;
    case text_format_GetReadingDirection: m.set_return(state.reading_direction); return;
    case text_format_GetFlowDirection: m.set_return(state.flow_direction); return;
    case text_format_GetIncrementalTabStop:
        // Learn: the default is four times the font size.
        m.set_return_f32(state.incremental_tab_stop > 0 ? state.incremental_tab_stop
                                                         : state.format.size * 4);
        return;
    case text_format_GetTrimming: {
        const GuestAddr options = m.arg_ptr(1);
        const GuestAddr sign = m.arg_ptr(2);
        const std::array<u32, 3> trimming{state.trimming_granularity, state.trimming_delimiter,
                                          state.trimming_delimiter_count};
        if (options == 0 || sign == 0 ||
            m_memory.write_bytes(options, reinterpret_cast<const Byte*>(trimming.data()), sizeof(trimming)).is_err() ||
            m.write_ptr(sign, state.trimming_sign).is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        retain_com(state.trimming_sign);
        m.set_return(S_OK);
        return;
    }
    case text_format_GetLineSpacing: {
        u32 spacing_bits = 0, baseline_bits = 0;
        std::memcpy(&spacing_bits, &state.line_spacing, 4);
        std::memcpy(&baseline_bits, &state.baseline, 4);
        if (m.arg_ptr(1) == 0 || m.arg_ptr(2) == 0 || m.arg_ptr(3) == 0 ||
            m.write_u32(m.arg_ptr(1), state.line_spacing_method).is_err() ||
            m.write_u32(m.arg_ptr(2), spacing_bits).is_err() ||
            m.write_u32(m.arg_ptr(3), baseline_bits).is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        m.set_return(S_OK);
        return;
    }
    case text_format_GetFontCollection: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || m.write_ptr(out, state.format.collection).is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        retain_com(state.format.collection);
        m.set_return(S_OK);
        return;
    }
    case text_format_GetFontFamilyNameLength:
        m.set_return(static_cast<u32>(state.format.family.size()));
        return;
    case text_format_GetFontFamilyName:
        m.set_return(write_name(m, m.arg_ptr(1), static_cast<u32>(m.arg(2)), state.format.family));
        return;
    case text_format_GetFontWeight: m.set_return(state.format.weight); return;
    case text_format_GetFontStyle: m.set_return(state.format.style); return;
    case text_format_GetFontStretch: m.set_return(state.format.stretch); return;
    case text_format_GetFontSize: m.set_return_f32(state.format.size); return;
    case text_format_GetLocaleNameLength:
        m.set_return(static_cast<u32>(state.format.locale.size()));
        return;
    case text_format_GetLocaleName:
        m.set_return(write_name(m, m.arg_ptr(1), static_cast<u32>(m.arg(2)), state.format.locale));
        return;
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

namespace {

/// Applies @p change to the formatting of [start, start+length) over the
/// whole position space (not clamped to the text).
template <typename Change>
void apply_range(DWriteManager::TextState& state, Range range, Change change) {
    if (range.length == 0) return;
    const u64 end = u64{range.start} + range.length;  // <= 2^32 (checked by caller)
    using Run = DWriteManager::TextState::StyleRun;
    const auto split = [&](u64 at) {
        if (at > 0xFFFFFFFFull) return;
        auto it = std::upper_bound(state.runs.begin(), state.runs.end(), static_cast<u32>(at),
                                   [](u32 p, const Run& r) { return p < r.start; });
        const Run& containing = *std::prev(it);
        if (containing.start != at) state.runs.insert(it, Run{static_cast<u32>(at), containing.spec});
    };
    split(range.start);
    split(end);
    for (auto& run : state.runs) {
        if (run.start >= range.start && run.start < end) change(run.spec);
    }
    std::vector<Run> merged;
    for (auto& run : state.runs) {
        if (!merged.empty() && merged.back().spec == run.spec) continue;
        merged.push_back(std::move(run));
    }
    state.runs = std::move(merged);
    state.invalidate();
}

}  // namespace

void DWriteManager::text_layout_method(Marshaler& m, GuestAddr self, TextState& state, u32 slot) {
    // Range setters: (value, DWRITE_TEXT_RANGE). The value is argument 1.
    const auto set_range = [&](auto change, bool references) {
        const Range range = read_range(m, 2);
        // dwrite_text_layout_probe: a range past UINT32 fails; empty is S_OK.
        if (u64{range.start} + range.length > 0x100000000ull) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const auto before = references ? held_references(state) : std::set<GuestAddr>{};
        apply_range(state, range, change);
        if (references) {
            const auto after = held_references(state);
            for (const GuestAddr object : after)
                if (!before.contains(object)) retain_com(object);
            for (const GuestAddr object : before)
                if (!after.contains(object)) release_com(object);
        }
        m.set_return(S_OK);
    };
    // Range getters: (position, out..., DWRITE_TEXT_RANGE* range).
    const auto spec_at = [&](u32 position) -> const TextStyleSpec* {
        return &state.spec_at(position);
    };
    // The formatting run holding @p position, whatever property is asked
    // (probe: after SetFontSize({6,5}), GetFontFamilyNameLength(0) reports
    // {0,6}); the last run reaches 0xFFFFFFFF ({11, 0xFFFFFFF4}).
    const auto write_range = [&](GuestAddr out, u32 position, auto) -> bool {
        if (out == 0) return true;
        using Run = TextState::StyleRun;
        auto first = std::prev(std::upper_bound(state.runs.begin(), state.runs.end(), position,
                                                [](u32 p, const Run& r) { return p < r.start; }));
        auto last = std::next(first);
        const u32 start = first->start;
        const u32 length = (last == state.runs.end() ? 0xFFFFFFFFu : last->start) - start;
        const u32 words[2]{start, length};
        return m_memory.write_bytes(out, reinterpret_cast<const Byte*>(words), sizeof(words)).is_ok();
    };
    switch (slot) {
    case text_layout_SetMaxWidth:
    case text_layout_SetMaxHeight: {
        const float value = read_float_argument(m, 1);
        if (std::isnan(value) || value < 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        (slot == text_layout_SetMaxWidth ? state.max_width : state.max_height) = value;
        state.invalidate();
        m.set_return(S_OK);
        return;
    }
    case text_layout_SetFontCollection: {
        const GuestAddr collection = m.arg_ptr(1);
        const auto chosen = m_objects.find(collection);
        if (collection != 0 && (chosen == m_objects.end() ||
                                chosen->second.type != ObjectType::FontCollection)) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const GuestAddr value = collection != 0 ? collection : state.format.collection;
        set_range([&](TextStyleSpec& s) { s.collection = value; }, true);
        return;
    }
    case text_layout_SetFontFamilyName:
    case text_layout_SetLocaleName: {
        const GuestAddr name = m.arg_ptr(1);
        auto value = name == 0 ? VResult<std::u16string>::err(Error::InvalidArgument)
                               : read_utf16_z(name, slot == text_layout_SetLocaleName ? 85 : 32768);
        if (value.is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const bool family = slot == text_layout_SetFontFamilyName;
        set_range([&](TextStyleSpec& s) { (family ? s.family : s.locale) = value.value(); }, false);
        return;
    }
    case text_layout_SetFontWeight:
    case text_layout_SetFontStyle:
    case text_layout_SetFontStretch: {
        const u32 value = static_cast<u32>(m.arg(1));
        const bool ok = slot == text_layout_SetFontWeight ? valid_weight(value)
                      : slot == text_layout_SetFontStyle ? valid_style(value) : valid_stretch(value);
        if (!ok) {
            m.set_return(E_INVALIDARG);
            return;
        }
        set_range([&](TextStyleSpec& s) {
            (slot == text_layout_SetFontWeight ? s.weight
             : slot == text_layout_SetFontStyle ? s.style : s.stretch) = value;
        }, false);
        return;
    }
    case text_layout_SetFontSize: {
        const float value = read_float_argument(m, 1);
        if (!valid_size(value)) {
            m.set_return(E_INVALIDARG);
            return;
        }
        set_range([&](TextStyleSpec& s) { s.size = value; }, false);
        return;
    }
    case text_layout_SetUnderline:
    case text_layout_SetStrikethrough: {
        const bool value = static_cast<u32>(m.arg(1)) != 0;
        set_range([&](TextStyleSpec& s) {
            (slot == text_layout_SetUnderline ? s.underline : s.strikethrough) = value;
        }, false);
        return;
    }
    case text_layout_SetDrawingEffect:
    case text_layout_SetInlineObject:
    case text_layout_SetTypography: {
        const GuestAddr value = m.arg_ptr(1);
        set_range([&](TextStyleSpec& s) {
            (slot == text_layout_SetDrawingEffect ? s.effect
             : slot == text_layout_SetInlineObject ? s.inline_object : s.typography) = value;
        }, true);
        return;
    }
    case text_layout_GetMaxWidth: m.set_return_f32(state.max_width); return;
    case text_layout_GetMaxHeight: m.set_return_f32(state.max_height); return;
    case text_layout_GetFontCollection_44:
    case text_layout_GetFontWeight_47:
    case text_layout_GetFontStyle_48:
    case text_layout_GetFontStretch_49:
    case text_layout_GetFontSize_50:
    case text_layout_GetUnderline:
    case text_layout_GetStrikethrough:
    case text_layout_GetDrawingEffect:
    case text_layout_GetInlineObject:
    case text_layout_GetTypography:
    case text_layout_GetFontFamilyNameLength_45:
    case text_layout_GetLocaleNameLength_56: {
        const u32 position = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        const GuestAddr range_out = m.arg_ptr(3);
        const TextStyleSpec* spec = spec_at(position);
        if (out == 0 || spec == nullptr) {
            m.set_return(E_INVALIDARG);
            return;
        }
        bool ok = true;
        GuestAddr reference = 0;
        switch (slot) {
        case text_layout_GetFontCollection_44:
            reference = spec->collection;
            ok = m.write_ptr(out, reference).is_ok();
            ok = ok && write_range(range_out, position, [](auto& a, auto& b) { return a.collection == b.collection; });
            break;
        case text_layout_GetFontWeight_47:
            ok = m.write_u32(out, spec->weight).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.weight == b.weight; });
            break;
        case text_layout_GetFontStyle_48:
            ok = m.write_u32(out, spec->style).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.style == b.style; });
            break;
        case text_layout_GetFontStretch_49:
            ok = m.write_u32(out, spec->stretch).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.stretch == b.stretch; });
            break;
        case text_layout_GetFontSize_50: {
            u32 bits = 0;
            std::memcpy(&bits, &spec->size, 4);
            ok = m.write_u32(out, bits).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.size == b.size; });
            break;
        }
        case text_layout_GetUnderline:
            ok = m.write_u32(out, spec->underline ? 1u : 0u).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.underline == b.underline; });
            break;
        case text_layout_GetStrikethrough:
            ok = m.write_u32(out, spec->strikethrough ? 1u : 0u).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.strikethrough == b.strikethrough; });
            break;
        case text_layout_GetDrawingEffect:
            reference = spec->effect;
            ok = m.write_ptr(out, reference).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.effect == b.effect; });
            break;
        case text_layout_GetInlineObject:
            reference = spec->inline_object;
            ok = m.write_ptr(out, reference).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.inline_object == b.inline_object; });
            break;
        case text_layout_GetTypography:
            reference = spec->typography;
            ok = m.write_ptr(out, reference).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.typography == b.typography; });
            break;
        case text_layout_GetFontFamilyNameLength_45:
            ok = m.write_u32(out, static_cast<u32>(spec->family.size())).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.family == b.family; });
            break;
        case text_layout_GetLocaleNameLength_56:
            ok = m.write_u32(out, static_cast<u32>(spec->locale.size())).is_ok() &&
                 write_range(range_out, position, [](auto& a, auto& b) { return a.locale == b.locale; });
            break;
        default:
            break;
        }
        if (!ok) {
            m.set_return(E_INVALIDARG);
            return;
        }
        retain_com(reference);
        m.set_return(S_OK);
        return;
    }
    case text_layout_GetFontFamilyName_46:
    case text_layout_GetLocaleName_57: {
        // (position, WCHAR* name, UINT32 nameSize, DWRITE_TEXT_RANGE* range)
        const u32 position = static_cast<u32>(m.arg(1));
        const TextStyleSpec* spec = spec_at(position);
        if (spec == nullptr) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const bool family = slot == text_layout_GetFontFamilyName_46;
        const u32 hr = write_name(m, m.arg_ptr(2), static_cast<u32>(m.arg(3)),
                                  family ? spec->family : spec->locale);
        if (hr == S_OK && !write_range(m.arg_ptr(4), position, [family](auto& a, auto& b) {
                return family ? a.family == b.family : a.locale == b.locale;
            })) {
            m.set_return(E_INVALIDARG);
            return;
        }
        m.set_return(hr);
        return;
    }
    default:
        m.set_return(text_layout_query(m, self, state, slot));
        return;
    }
}

}  // namespace vitreo::winapi
