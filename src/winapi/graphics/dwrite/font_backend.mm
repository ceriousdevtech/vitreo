// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/graphics/dwrite/font_backend_native.h"

#import <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

namespace vitreo::winapi::dwrite {
namespace {

std::u16string copy_cf_string(CFStringRef value) {
    if (value == nullptr) return {};
    const CFIndex length = CFStringGetLength(value);
    std::u16string result(static_cast<std::size_t>(length), u'\0');
    CFStringGetCharacters(
        value, CFRangeMake(0, length),
        reinterpret_cast<UniChar*>(result.data()));
    return result;
}

CTFontRef create_font(const std::u16string& name) {
    if (name.empty()) return nullptr;
    CFStringRef cf_name = CFStringCreateWithCharacters(
        kCFAllocatorDefault,
        reinterpret_cast<const UniChar*>(name.data()),
        static_cast<CFIndex>(name.size()));
    if (cf_name == nullptr) return nullptr;
    CTFontRef font = CTFontCreateWithName(cf_name, 1000.0, nullptr);
    CFRelease(cf_name);
    return font;
}

CTFontRef apply_traits(
    CTFontRef base, std::uint32_t weight, std::uint32_t stretch,
    std::uint32_t style) {
    if (base == nullptr) return nullptr;

    CTFontSymbolicTraits desired = 0;
    CTFontSymbolicTraits mask = 0;
    if (weight >= 600) {
        desired |= kCTFontBoldTrait;
        mask |= kCTFontBoldTrait;
    }
    if (style != 0) {
        desired |= kCTFontItalicTrait;
        mask |= kCTFontItalicTrait;
    }
    if (stretch < 5) {
        desired |= kCTFontCondensedTrait;
        mask |= kCTFontCondensedTrait;
    } else if (stretch > 5) {
        desired |= kCTFontExpandedTrait;
        mask |= kCTFontExpandedTrait;
    }
    if (mask == 0) return base;

    CTFontRef matched = CTFontCreateCopyWithSymbolicTraits(
        base, 1000.0, nullptr, desired, mask);
    if (matched == nullptr) return base;
    CFRelease(base);
    return matched;
}

template <typename T>
T clamp_metric(CGFloat value) {
    const double rounded = std::round(static_cast<double>(value));
    const double low = static_cast<double>(std::numeric_limits<T>::min());
    const double high = static_cast<double>(std::numeric_limits<T>::max());
    return static_cast<T>(std::clamp(rounded, low, high));
}

MatchedFont describe_font(
    CTFontRef font, bool system_ui_font = false,
    std::uint32_t face_index = 0) {
    const CTFontSymbolicTraits actual_traits = CTFontGetSymbolicTraits(font);
    MatchedFont result{};
    result.system_ui_font = system_ui_font;
    result.face_index = face_index;
    result.weight =
        (actual_traits & kCTFontBoldTrait) != 0 ? 700u : 400u;
    result.stretch =
        (actual_traits & kCTFontCondensedTrait) != 0 ? 3u
        : (actual_traits & kCTFontExpandedTrait) != 0 ? 7u
        : 5u;
    result.style =
        (actual_traits & kCTFontItalicTrait) != 0 ? 2u : 0u;
    if (CFStringRef family_name = CTFontCopyFamilyName(font)) {
        result.family_name = copy_cf_string(family_name);
        CFRelease(family_name);
    }
    if (CFStringRef face_name =
            CTFontCopyName(font, kCTFontSubFamilyNameKey)) {
        result.face_name = copy_cf_string(face_name);
        CFRelease(face_name);
    }
    if (CFStringRef postscript_name = CTFontCopyPostScriptName(font)) {
        result.postscript_name = copy_cf_string(postscript_name);
        CFRelease(postscript_name);
    }
    if (CTFontDescriptorRef descriptor = CTFontCopyFontDescriptor(font)) {
        auto url = static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(
            descriptor, kCTFontURLAttribute));
        if (url != nullptr) {
            if (CFStringRef path = CFURLCopyFileSystemPath(
                    url, kCFURLPOSIXPathStyle)) {
                const CFIndex length = CFStringGetLength(path);
                const CFIndex maximum = CFStringGetMaximumSizeForEncoding(
                    length, kCFStringEncodingUTF8) + 1;
                std::string utf8(static_cast<std::size_t>(maximum), '\0');
                if (CFStringGetCString(
                        path, utf8.data(), maximum,
                        kCFStringEncodingUTF8)) {
                    utf8.resize(std::strlen(utf8.c_str()));
                    result.source_path = std::move(utf8);
                }
                CFRelease(path);
            }
            CFRelease(url);
        }
        CFRelease(descriptor);
    }
    result.glyph_count = clamp_metric<std::uint16_t>(
        static_cast<CGFloat>(CTFontGetGlyphCount(font)));
    constexpr CTFontTableTag CFF_TABLE = 0x43464620u;
    constexpr CTFontTableTag GLYF_TABLE = 0x676c7966u;
    if (CFDataRef table =
            CTFontCopyTable(font, CFF_TABLE, kCTFontTableOptionNoOptions)) {
        result.face_type = 0; // DWRITE_FONT_FACE_TYPE_CFF
        CFRelease(table);
    } else if (CFDataRef table =
                   CTFontCopyTable(
                       font, GLYF_TABLE, kCTFontTableOptionNoOptions)) {
        result.face_type = 1; // DWRITE_FONT_FACE_TYPE_TRUETYPE
        CFRelease(table);
    }

    const std::uint16_t units =
        static_cast<std::uint16_t>(CTFontGetUnitsPerEm(font));
    const CGFloat point_size = CTFontGetSize(font);
    const CGFloat scale = point_size > 0
        ? static_cast<CGFloat>(units) / point_size
        : 1.0;
    result.metrics.design_units_per_em = units;
    result.metrics.ascent =
        clamp_metric<std::uint16_t>(CTFontGetAscent(font) * scale);
    result.metrics.descent =
        clamp_metric<std::uint16_t>(CTFontGetDescent(font) * scale);
    result.metrics.line_gap =
        clamp_metric<std::int16_t>(CTFontGetLeading(font) * scale);
    result.metrics.cap_height =
        clamp_metric<std::uint16_t>(CTFontGetCapHeight(font) * scale);
    result.metrics.x_height =
        clamp_metric<std::uint16_t>(CTFontGetXHeight(font) * scale);
    result.metrics.underline_position =
        clamp_metric<std::int16_t>(
            CTFontGetUnderlinePosition(font) * scale);
    result.metrics.underline_thickness =
        clamp_metric<std::uint16_t>(
            CTFontGetUnderlineThickness(font) * scale);
    result.metrics.strikethrough_position =
        clamp_metric<std::int16_t>(
            CTFontGetXHeight(font) * scale * 0.5);
    result.metrics.strikethrough_thickness =
        result.metrics.underline_thickness;
    return result;
}

CTFontRef create_font(const MatchedFont& matched) {
    if (matched.native_face) {
        return CTFontCreateCopyWithAttributes(
            static_cast<CTFontRef>(matched.native_face.get()),
            1000.0, nullptr, nullptr);
    }
    CTFontRef font = nullptr;
    if (matched.system_ui_font) {
        font = CTFontCreateUIFontForLanguage(
            kCTFontUIFontSystem, 1000.0, nullptr);
    } else if (!matched.postscript_name.empty() &&
               matched.postscript_name.front() != u'.') {
        font = create_font(matched.postscript_name);
    } else {
        // Dot-prefixed CoreText names (for example the system UI font's
        // private PostScript name) are not stable creation names. Reopening
        // one with CTFontCreateWithName can silently substitute a different
        // face, making DirectWrite metrics and glyph indices disagree.
        font = create_font(matched.family_name);
    }
    return apply_traits(
        font, matched.weight, matched.stretch, matched.style);
}

bool font_maps_scalar(
    CTFontRef font, const std::u16string& text, std::size_t offset,
    std::size_t* consumed) {
    if (offset >= text.size()) return false;
    std::size_t length = 1;
    const char16_t first = text[offset];
    if (first >= 0xd800 && first <= 0xdbff && offset + 1 < text.size()) {
        const char16_t second = text[offset + 1];
        if (second >= 0xdc00 && second <= 0xdfff) length = 2;
    }
    CGGlyph glyphs[2]{};
    const bool mapped = CTFontGetGlyphsForCharacters(
        font, reinterpret_cast<const UniChar*>(text.data() + offset), glyphs,
        static_cast<CFIndex>(length));
    if (consumed != nullptr) *consumed = length;
    return mapped;
}

// Windows' system UI family since Vista. macOS does not ship it, but Vitreo
// already tells guests it is the UI font through USER32 and GDI
// (NONCLIENTMETRICS.lfMessageFont, DEFAULT_GUI_FONT, SystemParametersInfo).
// DirectWrite has to be able to resolve the same name, or a guest that uses
// the font Vitreo just named gets whatever CoreText falls back to -- a
// different typeface at different metrics, with no error anywhere.
bool is_windows_ui_family(const std::u16string& family) {
    // DirectWrite family lookup is case-insensitive.
    static constexpr char16_t kSegoeUi[] = u"Segoe UI";
    constexpr std::size_t length = sizeof(kSegoeUi) / sizeof(char16_t) - 1;
    if (family.size() != length) return false;
    for (std::size_t i = 0; i < length; ++i) {
        char16_t a = family[i];
        char16_t b = kSegoeUi[i];
        if (a >= u'A' && a <= u'Z') a = static_cast<char16_t>(a + 32);
        if (b >= u'A' && b <= u'Z') b = static_cast<char16_t>(b + 32);
        if (a != b) return false;
    }
    return true;
}

} // namespace

CTFontRef create_native_font(const MatchedFont& font) { return create_font(font); }

MatchedFont describe_ct_font(CTFontRef font) { return describe_font(font); }

std::vector<std::u16string> enumerate_system_font_families() {
    std::vector<std::u16string> families;
    CFArrayRef names = CTFontManagerCopyAvailableFontFamilyNames();
    if (names == nullptr) return families;

    const CFIndex count = CFArrayGetCount(names);
    families.reserve(static_cast<std::size_t>(count));
    for (CFIndex index = 0; index < count; ++index) {
        auto name = static_cast<CFStringRef>(CFArrayGetValueAtIndex(names, index));
        std::u16string family = copy_cf_string(name);
        if (!family.empty()) families.push_back(std::move(family));
    }
    CFRelease(names);

    // Publish the Windows system UI family so FindFamilyName resolves it. A
    // host that really has the font keeps its own entry -- this only fills a
    // gap, it never shadows an installed face.
    if (std::find_if(families.begin(), families.end(), is_windows_ui_family) ==
        families.end()) {
        families.push_back(u"Segoe UI");
    }

    std::sort(families.begin(), families.end());
    families.erase(std::unique(families.begin(), families.end()), families.end());
    return families;
}

std::optional<MatchedFont> match_system_font(
    const std::u16string& family,
    std::uint32_t weight,
    std::uint32_t stretch,
    std::uint32_t style) {
    if (family.empty()) return std::nullopt;
    if (is_windows_ui_family(family) &&
        enumerate_system_family_fonts(family).empty()) {
        // Substitute the host UI font, but answer under the requested family
        // name: a guest that asked for this family must not be told it got a
        // different one. system_ui_font also keeps reopening stable, since the
        // host UI font's PostScript name is dot-prefixed and not a reliable
        // creation name.
        auto substitute = match_system_ui_font(weight, stretch, style);
        if (substitute) {
            substitute->family_name = family;
            return substitute;
        }
    }
    CTFontRef matched = apply_traits(
        create_font(family), weight, stretch, style);
    if (matched == nullptr) return std::nullopt;
    MatchedFont result = describe_font(matched);
    CFRelease(matched);
    return result;
}

std::optional<MatchedFont> match_system_ui_font(
    std::uint32_t weight,
    std::uint32_t stretch,
    std::uint32_t style) {
    CTFontRef matched = apply_traits(
        CTFontCreateUIFontForLanguage(
            kCTFontUIFontSystem, 1000.0, nullptr),
        weight, stretch, style);
    if (matched == nullptr) return std::nullopt;
    MatchedFont result = describe_font(matched, true);
    CFRelease(matched);
    return result;
}

std::vector<MatchedFont> enumerate_system_family_fonts(
    const std::u16string& family) {
    std::vector<MatchedFont> result;
    if (family.empty()) return result;

    CFStringRef family_name = CFStringCreateWithCharacters(
        kCFAllocatorDefault,
        reinterpret_cast<const UniChar*>(family.data()),
        static_cast<CFIndex>(family.size()));
    if (family_name == nullptr) return result;
    const void* keys[]{kCTFontFamilyNameAttribute};
    const void* values[]{family_name};
    CFDictionaryRef attributes = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(family_name);
    if (attributes == nullptr) return result;
    CTFontDescriptorRef query = CTFontDescriptorCreateWithAttributes(attributes);
    CFRelease(attributes);
    if (query == nullptr) return result;
    CFArrayRef descriptors = CTFontDescriptorCreateMatchingFontDescriptors(
        query, nullptr);
    CFRelease(query);
    if (descriptors == nullptr) return result;

    const CFIndex count = CFArrayGetCount(descriptors);
    result.reserve(static_cast<std::size_t>(count));
    for (CFIndex index = 0; index < count; ++index) {
        auto descriptor = static_cast<CTFontDescriptorRef>(
            CFArrayGetValueAtIndex(descriptors, index));
        CTFontRef font = CTFontCreateWithFontDescriptor(
            descriptor, 1000.0, nullptr);
        if (font == nullptr) continue;
        MatchedFont matched = describe_font(font);
        CFRelease(font);
        if (!matched.family_name.empty() && !matched.postscript_name.empty()) {
            result.push_back(std::move(matched));
        }
    }
    CFRelease(descriptors);
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.postscript_name < right.postscript_name;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.postscript_name == right.postscript_name;
    }), result.end());
    return result;
}

std::optional<FallbackMatch> map_system_font(
    const std::u16string& text,
    const std::u16string& base_family,
    std::uint32_t weight,
    std::uint32_t stretch,
    std::uint32_t style,
    const std::u16string& locale) {
    if (text.empty()) return FallbackMatch{};

    CFStringRef language = nullptr;
    if (!locale.empty()) {
        language = CFStringCreateWithCharacters(
            kCFAllocatorDefault,
            reinterpret_cast<const UniChar*>(locale.data()),
            static_cast<CFIndex>(locale.size()));
    }
    CTFontRef base = base_family.empty()
        ? CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 1000.0, language)
        : create_font(base_family);
    if (language != nullptr) CFRelease(language);
    base = apply_traits(base, weight, stretch, style);
    if (base == nullptr) return std::nullopt;

    CFStringRef string = CFStringCreateWithCharacters(
        kCFAllocatorDefault,
        reinterpret_cast<const UniChar*>(text.data()),
        static_cast<CFIndex>(text.size()));
    if (string == nullptr) {
        CFRelease(base);
        return std::nullopt;
    }

    std::size_t first_length = 0;
    CTFontRef selected = nullptr;
    bool selected_is_system_ui = false;
    if (font_maps_scalar(base, text, 0, &first_length)) {
        selected = static_cast<CTFontRef>(CFRetain(base));
        selected_is_system_ui = base_family.empty();
    } else {
        selected = CTFontCreateForString(
            base, string, CFRangeMake(0, static_cast<CFIndex>(first_length)));
    }
    CFRelease(string);
    CFRelease(base);
    if (selected == nullptr) {
        FallbackMatch result{};
        result.mapped_length = static_cast<std::uint32_t>(first_length);
        return result;
    }

    std::size_t selected_first_length = 0;
    if (!font_maps_scalar(selected, text, 0, &selected_first_length)) {
        CFRelease(selected);
        FallbackMatch result{};
        result.mapped_length = static_cast<std::uint32_t>(first_length);
        return result;
    }

    std::uint32_t mapped_length = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        std::size_t scalar_length = 0;
        if (!font_maps_scalar(selected, text, offset, &scalar_length)) break;
        offset += scalar_length;
        mapped_length = static_cast<std::uint32_t>(offset);
    }

    FallbackMatch result{};
    result.mapped_length = mapped_length != 0
        ? mapped_length
        : static_cast<std::uint32_t>(first_length);
    result.has_font = true;
    result.font = describe_font(selected, selected_is_system_ui);
    result.family_name = result.font.family_name;
    CFRelease(selected);
    if (result.mapped_length == 0 || result.font.postscript_name.empty()) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::vector<std::uint16_t>> glyph_indices(
    const MatchedFont& font,
    const std::vector<std::uint32_t>& codepoints) {
    CTFontRef native_font = create_font(font);
    if (native_font == nullptr) return std::nullopt;

    std::vector<std::uint16_t> result;
    result.reserve(codepoints.size());
    for (const std::uint32_t codepoint : codepoints) {
        if (codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
            result.push_back(0);
            continue;
        }
        if (codepoint <= 0xffffu) {
            const UniChar character = static_cast<UniChar>(codepoint);
            CGGlyph glyph = 0;
            (void)CTFontGetGlyphsForCharacters(
                native_font, &character, &glyph, 1);
            result.push_back(static_cast<std::uint16_t>(glyph));
            continue;
        }

        const std::uint32_t scalar = codepoint - 0x10000u;
        const UniChar characters[2]{
            static_cast<UniChar>(0xd800u + (scalar >> 10)),
            static_cast<UniChar>(0xdc00u + (scalar & 0x3ffu)),
        };
        CGGlyph glyphs[2]{};
        (void)CTFontGetGlyphsForCharacters(
            native_font, characters, glyphs, 2);
        result.push_back(static_cast<std::uint16_t>(
            glyphs[0] != 0 ? glyphs[0] : glyphs[1]));
    }

    CFRelease(native_font);
    return result;
}

std::optional<std::vector<GlyphMetrics>> glyph_metrics(
    const MatchedFont& font,
    const std::vector<std::uint16_t>& glyph_indices) {
    CTFontRef native_font = create_font(font);
    if (native_font == nullptr) return std::nullopt;

    const CGFloat point_size = CTFontGetSize(native_font);
    const CGFloat scale = point_size > 0
        ? static_cast<CGFloat>(CTFontGetUnitsPerEm(native_font)) / point_size
        : 1.0;
    std::vector<CGGlyph> native_glyphs(
        glyph_indices.begin(), glyph_indices.end());
    std::vector<CGSize> horizontal_advances(native_glyphs.size());
    std::vector<CGSize> vertical_advances(native_glyphs.size());
    std::vector<CGRect> bounds(native_glyphs.size());
    std::vector<CGSize> vertical_translations(native_glyphs.size());
    if (!native_glyphs.empty()) {
        const CFIndex count = static_cast<CFIndex>(native_glyphs.size());
        (void)CTFontGetAdvancesForGlyphs(
            native_font, kCTFontOrientationHorizontal,
            native_glyphs.data(), horizontal_advances.data(), count);
        (void)CTFontGetAdvancesForGlyphs(
            native_font, kCTFontOrientationVertical,
            native_glyphs.data(), vertical_advances.data(), count);
        (void)CTFontGetBoundingRectsForGlyphs(
            native_font, kCTFontOrientationHorizontal,
            native_glyphs.data(), bounds.data(), count);
        CTFontGetVerticalTranslationsForGlyphs(
            native_font, native_glyphs.data(),
            vertical_translations.data(), count);
    }

    std::vector<GlyphMetrics> result;
    result.reserve(native_glyphs.size());
    for (std::size_t index = 0; index < native_glyphs.size(); ++index) {
        const CGRect box = CGRectIsNull(bounds[index])
            ? CGRectZero
            : bounds[index];
        const CGFloat left = CGRectGetMinX(box) * scale;
        const CGFloat width = CGRectGetWidth(box) * scale;
        const CGFloat advance_width =
            std::max<CGFloat>(0, horizontal_advances[index].width * scale);
        // CoreText reports the default-horizontal to vertical-origin
        // translation with a negative Y value, while DirectWrite's
        // verticalOriginY is positive above the horizontal baseline.
        const CGFloat vertical_origin_y =
            -vertical_translations[index].height * scale;
        const CGFloat top =
            vertical_origin_y - CGRectGetMaxY(box) * scale;
        const CGFloat advance_height = std::max<CGFloat>(
            0, vertical_advances[index].width * scale);

        GlyphMetrics metrics{};
        metrics.left_side_bearing = clamp_metric<std::int32_t>(left);
        metrics.advance_width =
            clamp_metric<std::uint32_t>(advance_width);
        metrics.right_side_bearing = clamp_metric<std::int32_t>(
            advance_width - left - width);
        metrics.top_side_bearing = clamp_metric<std::int32_t>(top);
        metrics.advance_height =
            clamp_metric<std::uint32_t>(advance_height);
        metrics.bottom_side_bearing = clamp_metric<std::int32_t>(
            advance_height - top - CGRectGetHeight(box) * scale);
        metrics.vertical_origin_y =
            clamp_metric<std::int32_t>(vertical_origin_y);
        result.push_back(metrics);
    }

    CFRelease(native_font);
    return result;
}

std::optional<RasterizedGlyphRun> rasterize_glyph_run(
    const GlyphRunRasterParams& params, bool antialias) {
    if (!(params.font_em_size > 0) || !(params.pixels_per_dip > 0) ||
        params.is_sideways ||
        (!params.glyph_advances.empty() &&
         params.glyph_advances.size() != params.glyph_indices.size()) ||
        (!params.glyph_offsets.empty() &&
         params.glyph_offsets.size() != params.glyph_indices.size())) {
        return std::nullopt;
    }

    CTFontRef base_font = create_font(params.font);
    if (base_font == nullptr) return std::nullopt;
    const CGFloat pixel_em_size =
        static_cast<CGFloat>(params.font_em_size * params.pixels_per_dip);
    CTFontRef native_font = CTFontCreateCopyWithAttributes(
        base_font, pixel_em_size, nullptr, nullptr);
    CFRelease(base_font);
    if (native_font == nullptr) return std::nullopt;

    std::vector<CGGlyph> glyphs(
        params.glyph_indices.begin(), params.glyph_indices.end());
    std::vector<CGSize> default_advances(glyphs.size());
    if (params.glyph_advances.empty() && !glyphs.empty()) {
        (void)CTFontGetAdvancesForGlyphs(
            native_font, kCTFontOrientationHorizontal, glyphs.data(),
            default_advances.data(), static_cast<CFIndex>(glyphs.size()));
    }

    const CGFloat m11 = params.transform[0];
    const CGFloat m12 = params.transform[1];
    const CGFloat m21 = params.transform[2];
    const CGFloat m22 = params.transform[3];
    const CGFloat dx = params.transform[4];
    const CGFloat dy = params.transform[5];
    const CGFloat baseline_x =
        params.baseline_origin_x * params.pixels_per_dip;
    const CGFloat baseline_y =
        params.baseline_origin_y * params.pixels_per_dip;
    const CGFloat direction = (params.bidi_level & 1u) != 0 ? -1.0 : 1.0;

    std::vector<CGPathRef> paths;
    paths.reserve(glyphs.size());
    CGRect run_bounds = CGRectNull;
    CGFloat pen = 0;
    for (std::size_t index = 0; index < glyphs.size(); ++index) {
        const GlyphOffset offset = params.glyph_offsets.empty()
            ? GlyphOffset{}
            : params.glyph_offsets[index];
        const CGFloat glyph_origin_x = baseline_x + direction *
            (pen + offset.advance_offset * params.pixels_per_dip);
        const CGFloat glyph_origin_y = baseline_y -
            offset.ascender_offset * params.pixels_per_dip;
        const CGAffineTransform transform{
            m11,
            m12,
            -m21,
            -m22,
            m11 * glyph_origin_x + m21 * glyph_origin_y + dx,
            m12 * glyph_origin_x + m22 * glyph_origin_y + dy,
        };
        CGPathRef native_path =
            CTFontCreatePathForGlyph(native_font, glyphs[index], nullptr);
        CGPathRef path = native_path != nullptr
            ? CGPathCreateCopyByTransformingPath(native_path, &transform)
            : nullptr;
        if (native_path != nullptr) CGPathRelease(native_path);
        if (path != nullptr) {
            const CGRect bounds = CGPathGetBoundingBox(path);
            run_bounds = CGRectIsNull(run_bounds)
                ? bounds
                : CGRectUnion(run_bounds, bounds);
            paths.push_back(path);
        }

        const CGFloat advance = params.glyph_advances.empty()
            ? default_advances[index].width
            : params.glyph_advances[index] * params.pixels_per_dip;
        pen += advance;
    }
    CFRelease(native_font);

    RasterizedGlyphRun result{};
    if (CGRectIsNull(run_bounds)) return result;
    const double left = std::floor(CGRectGetMinX(run_bounds));
    const double top = std::floor(CGRectGetMinY(run_bounds));
    const double right = std::ceil(CGRectGetMaxX(run_bounds));
    const double bottom = std::ceil(CGRectGetMaxY(run_bounds));
    result.left = clamp_metric<std::int32_t>(left);
    result.top = clamp_metric<std::int32_t>(top);
    result.right = clamp_metric<std::int32_t>(right);
    result.bottom = clamp_metric<std::int32_t>(bottom);
    const std::int64_t width =
        static_cast<std::int64_t>(result.right) - result.left;
    const std::int64_t height =
        static_cast<std::int64_t>(result.bottom) - result.top;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        width * height > 64 * 1024 * 1024) {
        for (CGPathRef path : paths) CGPathRelease(path);
        return std::nullopt;
    }

    result.alpha.assign(
        static_cast<std::size_t>(width * height), std::uint8_t{0});
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceGray();
    CGContextRef context = color_space != nullptr
        ? CGBitmapContextCreate(
              result.alpha.data(), static_cast<std::size_t>(width),
              static_cast<std::size_t>(height), 8,
              static_cast<std::size_t>(width), color_space,
              kCGImageAlphaNone)
        : nullptr;
    if (color_space != nullptr) CGColorSpaceRelease(color_space);
    if (context == nullptr) {
        for (CGPathRef path : paths) CGPathRelease(path);
        return std::nullopt;
    }
    CGContextSetShouldAntialias(context, antialias);
    CGContextSetAllowsAntialiasing(context, antialias);
    CGContextSetShouldSmoothFonts(context, antialias);
    CGContextSetAllowsFontSmoothing(context, antialias);
    CGContextSetGrayFillColor(context, 1.0, 1.0);
    CGContextTranslateCTM(context, -result.left, -result.top);
    for (CGPathRef path : paths) {
        CGContextAddPath(context, path);
        CGContextFillPath(context);
        CGPathRelease(path);
    }
    CGContextFlush(context);
    CGContextRelease(context);
    // CoreGraphics addresses the bitmap from a lower-left origin, whereas
    // DirectWrite's alpha texture is row-major from textureBounds.top.
    // Convert the host bitmap to the guest's top-down row order.
    const std::size_t row_size = static_cast<std::size_t>(width);
    for (std::size_t top_row = 0,
                     bottom_row = static_cast<std::size_t>(height) - 1;
         top_row < bottom_row; ++top_row, --bottom_row) {
        for (std::size_t column = 0; column < row_size; ++column) {
            std::swap(
                result.alpha[top_row * row_size + column],
                result.alpha[bottom_row * row_size + column]);
        }
    }
    return result;
}

namespace {

struct PathApplyState {
    float origin_x{0};
    float origin_y{0};
    GlyphRunOutline* outline{nullptr};
    OutlineFigure figure{};
    bool in_figure{false};

    OutlinePoint map(CGPoint point) const {
        // CoreText glyph paths are Y-up. Emit +Y-down sink coordinates so
        // ascenders are negative Y (above the baseline) in the geometry the
        // guest records — matching the orientation CreateGlyphRunAnalysis
        // alpha textures use after the DWrite→device conversion.
        return {
            static_cast<float>(point.x) + origin_x,
            origin_y - static_cast<float>(point.y),
        };
    }

    void finish_figure(bool closed) {
        if (!in_figure) return;
        figure.closed = closed;
        outline->figures.push_back(std::move(figure));
        figure = OutlineFigure{};
        in_figure = false;
    }
};

void path_apply(void* info, const CGPathElement* element) {
    auto* state = static_cast<PathApplyState*>(info);
    switch (element->type) {
    case kCGPathElementMoveToPoint: {
        state->finish_figure(false);
        state->figure.start = state->map(element->points[0]);
        state->in_figure = true;
        break;
    }
    case kCGPathElementAddLineToPoint: {
        if (!state->in_figure) {
            state->figure.start = state->map(element->points[0]);
            state->in_figure = true;
            break;
        }
        OutlineSegment segment{};
        segment.kind = OutlineSegmentKind::Line;
        segment.line = state->map(element->points[0]);
        state->figure.segments.push_back(segment);
        break;
    }
    case kCGPathElementAddQuadCurveToPoint: {
        if (!state->in_figure) break;
        // Elevate quadratic (P0,Q,P2) to cubic for IDWriteGeometrySink.
        OutlinePoint current = state->figure.start;
        if (!state->figure.segments.empty()) {
            const auto& last = state->figure.segments.back();
            current = last.kind == OutlineSegmentKind::Line
                ? last.line
                : last.bezier.end;
        }
        const OutlinePoint control = state->map(element->points[0]);
        const OutlinePoint end = state->map(element->points[1]);
        OutlineSegment segment{};
        segment.kind = OutlineSegmentKind::Bezier;
        segment.bezier.control1 = {
            current.x + (2.0f / 3.0f) * (control.x - current.x),
            current.y + (2.0f / 3.0f) * (control.y - current.y),
        };
        segment.bezier.control2 = {
            end.x + (2.0f / 3.0f) * (control.x - end.x),
            end.y + (2.0f / 3.0f) * (control.y - end.y),
        };
        segment.bezier.end = end;
        state->figure.segments.push_back(segment);
        break;
    }
    case kCGPathElementAddCurveToPoint: {
        if (!state->in_figure) break;
        OutlineSegment segment{};
        segment.kind = OutlineSegmentKind::Bezier;
        segment.bezier.control1 = state->map(element->points[0]);
        segment.bezier.control2 = state->map(element->points[1]);
        segment.bezier.end = state->map(element->points[2]);
        state->figure.segments.push_back(segment);
        break;
    }
    case kCGPathElementCloseSubpath:
        state->finish_figure(true);
        break;
    }
}

} // namespace

std::optional<GlyphRunOutline> glyph_run_outline(
    const GlyphOutlineParams& params) {
    if (!(params.em_size > 0) || !std::isfinite(params.em_size) ||
        (!params.glyph_advances.empty() &&
         params.glyph_advances.size() != params.glyph_indices.size()) ||
        (!params.glyph_offsets.empty() &&
         params.glyph_offsets.size() != params.glyph_indices.size()) ||
        params.glyph_indices.size() > 4096) {
        return std::nullopt;
    }

    CTFontRef base_font = create_font(params.font);
    if (base_font == nullptr) return std::nullopt;
    CTFontRef native_font = CTFontCreateCopyWithAttributes(
        base_font, static_cast<CGFloat>(params.em_size), nullptr, nullptr);
    CFRelease(base_font);
    if (native_font == nullptr) return std::nullopt;

    std::vector<CGGlyph> glyphs(
        params.glyph_indices.begin(), params.glyph_indices.end());
    std::vector<CGSize> default_advances(glyphs.size());
    if (params.glyph_advances.empty() && !glyphs.empty()) {
        (void)CTFontGetAdvancesForGlyphs(
            native_font, kCTFontOrientationHorizontal, glyphs.data(),
            default_advances.data(), static_cast<CFIndex>(glyphs.size()));
    }

    GlyphRunOutline outline{};
    const float direction = params.is_right_to_left ? -1.0f : 1.0f;
    float pen = 0;
    for (std::size_t index = 0; index < glyphs.size(); ++index) {
        const GlyphOffset offset = params.glyph_offsets.empty()
            ? GlyphOffset{}
            : params.glyph_offsets[index];
        const float origin_x =
            direction * (pen + offset.advance_offset);
        // DWRITE_GLYPH_OFFSET.ascenderOffset: positive moves the glyph up.
        // Sink space here is +Y-down, so apply the opposite sign.
        const float origin_y = -offset.ascender_offset;

        CGPathRef path =
            CTFontCreatePathForGlyph(native_font, glyphs[index], nullptr);
        if (path != nullptr) {
            PathApplyState state{};
            state.origin_x = origin_x;
            state.origin_y = origin_y;
            state.outline = &outline;
            CGPathApply(path, &state, path_apply);
            state.finish_figure(false);
            CGPathRelease(path);
        }

        const float advance = params.glyph_advances.empty()
            ? static_cast<float>(default_advances[index].width)
            : params.glyph_advances[index];
        if (!std::isfinite(advance)) {
            CFRelease(native_font);
            return std::nullopt;
        }
        pen += advance;
    }
    CFRelease(native_font);
    return outline;
}

std::optional<std::vector<std::uint8_t>> font_table(
    const MatchedFont& font, std::uint32_t open_type_tag) {
    CTFontRef native_font = create_font(font);
    if (native_font == nullptr) return std::nullopt;

    // DWRITE_MAKE_OPENTYPE_TAG places the first character in the low byte,
    // while CTFontTableTag uses the conventional big-endian four-character
    // code (for example, DirectWrite cmap=0x70616d63 and CoreText
    // cmap=0x636d6170).
    const CTFontTableTag core_text_tag =
        ((open_type_tag & 0x000000ffu) << 24) |
        ((open_type_tag & 0x0000ff00u) << 8) |
        ((open_type_tag & 0x00ff0000u) >> 8) |
        ((open_type_tag & 0xff000000u) >> 24);
    CFDataRef table = CTFontCopyTable(
        native_font, core_text_tag, kCTFontTableOptionNoOptions);
    CFRelease(native_font);
    if (table == nullptr) return std::vector<std::uint8_t>{};

    const CFIndex length = CFDataGetLength(table);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (length != 0) {
        CFDataGetBytes(
            table, CFRangeMake(0, length),
            reinterpret_cast<UInt8*>(bytes.data()));
    }
    CFRelease(table);
    return bytes;
}

std::optional<std::vector<std::uint8_t>> font_file_data(
    const MatchedFont& font) {
    if (font.source_data) return *font.source_data;
    if (font.source_path.empty()) return std::nullopt;
    std::ifstream input(font.source_path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const std::streamoff end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) >
                       std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty() &&
        !input.read(reinterpret_cast<char*>(data.data()), end)) {
        return std::nullopt;
    }
    return data;
}

std::vector<MatchedFont> fonts_from_data(
    const std::vector<std::uint8_t>& data) {
    if (data.empty() || data.size() >
                            static_cast<std::size_t>(
                                std::numeric_limits<CFIndex>::max())) {
        return {};
    }
    return fonts_from_data(
        std::make_shared<const std::vector<std::uint8_t>>(data));
}

std::vector<MatchedFont> fonts_from_data(
    std::shared_ptr<const std::vector<std::uint8_t>> owned_data) {
    std::vector<MatchedFont> result;
    if (!owned_data || owned_data->empty() ||
        owned_data->size() > static_cast<std::size_t>(
                                 std::numeric_limits<CFIndex>::max())) {
        return result;
    }
    // No-copy: `owned_data` is retained in every MatchedFont::source_data
    // below precisely so the bytes outlive the CTFont, so CoreText can borrow
    // them. Copying here doubled peak memory for the whole blob — 2 x 188 MB
    // live simultaneously on a large custom collection. kCFAllocatorNull
    // leaves ownership with the shared_ptr.
    CFDataRef font_data = CFDataCreateWithBytesNoCopy(
        kCFAllocatorDefault, owned_data->data(),
        static_cast<CFIndex>(owned_data->size()), kCFAllocatorNull);
    if (font_data == nullptr) return result;
    CFArrayRef descriptors = CTFontManagerCreateFontDescriptorsFromData(font_data);
    CFRelease(font_data);
    if (descriptors == nullptr) return result;
    const CFIndex count = CFArrayGetCount(descriptors);
    result.reserve(static_cast<std::size_t>(count));
    for (CFIndex index = 0; index < count; ++index) {
        auto descriptor = static_cast<CTFontDescriptorRef>(
            CFArrayGetValueAtIndex(descriptors, index));
        CTFontRef font = CTFontCreateWithFontDescriptor(
            descriptor, 1000.0, nullptr);
        if (font == nullptr) continue;
        MatchedFont matched = describe_font(
            font, false, static_cast<std::uint32_t>(index));
        // A custom collection's file identity is owned by its loader. Do not
        // leak a CoreText URL that may have been inferred while parsing data.
        matched.source_path.clear();
        matched.source_data = owned_data;
        matched.native_face = std::shared_ptr<void>(
            const_cast<void*>(static_cast<const void*>(font)),
            [](void* value) {
                CFRelease(static_cast<CFTypeRef>(value));
            });
        if (!matched.family_name.empty() && !matched.postscript_name.empty()) {
            result.push_back(std::move(matched));
        }
    }
    CFRelease(descriptors);
    return result;
}

} // namespace vitreo::winapi::dwrite
