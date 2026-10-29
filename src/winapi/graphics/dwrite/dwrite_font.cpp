// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/dwrite/dwrite_internal.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace vitreo::winapi {
namespace {

/// Face name of a simulated font, as dwrite_gdi_interop_probe shows
/// DirectWrite composing it: emboldening drops the base name's weight words
/// and appends "Bold" ("Light Italic" -> "Italic Bold"); oblique drops
/// "Regular" and appends "Oblique" ("Bold" -> "Bold Oblique").
std::u16string simulated_face_name(const std::u16string& base,
                                   u32 simulations) {
    if (simulations == 0) return base;
    static const std::u16string_view weight_words[]{
        u"thin", u"extralight", u"ultralight", u"light", u"semilight",
        u"normal", u"regular", u"medium", u"demibold", u"semibold",
        u"bold", u"extrabold", u"ultrabold", u"black", u"heavy",
        u"extrablack", u"ultrablack",
    };
    std::vector<std::u16string> words;
    for (usize start = 0; start < base.size();) {
        const usize end = std::min(base.find(u' ', start), base.size());
        if (end > start) words.push_back(base.substr(start, end - start));
        start = end + 1;
    }
    const auto drop = [&](auto&& predicate) {
        words.erase(std::remove_if(words.begin(), words.end(), predicate),
                    words.end());
    };
    if ((simulations & DWRITE_FONT_SIMULATIONS_BOLD) != 0) {
        drop([&](const std::u16string& word) {
            return std::find(std::begin(weight_words), std::end(weight_words),
                             fold_ascii(word)) != std::end(weight_words);
        });
        words.push_back(u"Bold");
    }
    if ((simulations & DWRITE_FONT_SIMULATIONS_OBLIQUE) != 0) {
        drop([](const std::u16string& word) {
            return fold_ascii(word) == u"regular";
        });
        words.push_back(u"Oblique");
    }
    std::u16string name;
    for (const auto& word : words) {
        if (!name.empty()) name.push_back(u' ');
        name += word;
    }
    return name;
}

std::u16string face_display_name(const dwrite::MatchedFont& font,
                                 u32 simulations) {
    if (!font.face_name.empty()) {
        return simulated_face_name(font.face_name, simulations);
    }
    const bool bold = font.weight >= 600;
    const bool italic = font.style != 0;
    if (bold && italic) return simulated_face_name(u"Bold Italic", simulations);
    if (bold) return simulated_face_name(u"Bold", simulations);
    if (italic) return simulated_face_name(u"Italic", simulations);
    return simulated_face_name(u"Regular", simulations);
}

} // namespace

void DWriteManager::impl_FontGetFamily(Marshaler& m) {
    const auto font = m_objects.find(m.arg_ptr(0));
    const GuestAddr family_out = m.arg_ptr(1);
    if (family_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(family_out, 0);
    if (font == m_objects.end() || font->second.type != ObjectType::Font) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr family =
        create_object(ObjectType::FontFamily, font->second.family_index);
    if (family == 0 || m.write_ptr(family_out, family).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m_objects.at(family).family_fonts = font->second.family_fonts;
    m.set_return(S_OK);
}

void DWriteManager::impl_FontGetWeight(Marshaler& m) {
    const auto font = m_objects.find(m.arg_ptr(0));
    m.set_return(font != m_objects.end() && font->second.type == ObjectType::Font
        ? reported_weight(font->second.font, font->second.simulations) : 0);
}

void DWriteManager::impl_FontGetStretch(Marshaler& m) {
    const auto font = m_objects.find(m.arg_ptr(0));
    m.set_return(font != m_objects.end() && font->second.type == ObjectType::Font
        ? font->second.font.stretch : 0);
}

void DWriteManager::impl_FontGetStyle(Marshaler& m) {
    const auto font = m_objects.find(m.arg_ptr(0));
    m.set_return(font != m_objects.end() && font->second.type == ObjectType::Font
        ? reported_style(font->second.font, font->second.simulations) : 0);
}

void DWriteManager::impl_FontIsSymbolFont(Marshaler& m) {
    (void)m.arg_ptr(0);
    m.set_return(0);
}

void DWriteManager::impl_FontGetFaceNames(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr names_out = m.arg_ptr(1);
    if (names_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(names_out, 0);
    const auto font = m_objects.find(self);
    if (font == m_objects.end() || font->second.type != ObjectType::Font) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr names = create_object(
        ObjectType::LocalizedStrings, font->second.family_index);
    if (names == 0 || m.write_ptr(names_out, names).is_err()) {
        if (names != 0) m_objects.erase(names);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    auto& value = m_objects.at(names).localized_value;
    value = font->second.font.face_name;
    if (value.empty()) {
        const bool bold = font->second.font.weight >= 600;
        const bool italic = font->second.font.style != 0;
        value = bold && italic ? u"Bold Italic"
              : bold ? u"Bold"
              : italic ? u"Italic"
              : u"Regular";
    }
    value = simulated_face_name(value, font->second.simulations);
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] Font::GetFaceNames font=0x%llx "
                     "-> S_OK names=0x%llx\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(names));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FontGetSimulations(Marshaler& m) {
    // Only CreateFontFromLOGFONT simulates: every other path resolves a
    // native CoreText face as it is.
    const auto font = m_objects.find(m.arg_ptr(0));
    m.set_return(font != m_objects.end() && font->second.type == ObjectType::Font
        ? font->second.simulations : 0u);
}

void DWriteManager::impl_FontGetMetrics(Marshaler& m) {
    const auto font = m_objects.find(m.arg_ptr(0));
    const GuestAddr out = m.arg_ptr(1);
    if (font == m_objects.end() || font->second.type != ObjectType::Font ||
        out == 0) {
        return;
    }
    const auto& metrics = font->second.font.metrics;
    const std::array<u16, 10> fields{
        metrics.design_units_per_em,
        metrics.ascent,
        metrics.descent,
        static_cast<u16>(metrics.line_gap),
        metrics.cap_height,
        metrics.x_height,
        static_cast<u16>(metrics.underline_position),
        metrics.underline_thickness,
        static_cast<u16>(metrics.strikethrough_position),
        metrics.strikethrough_thickness,
    };
    for (usize i = 0; i < fields.size(); ++i) {
        (void)m.write_u16(out + i * sizeof(u16), fields[i]);
    }
}

void DWriteManager::impl_FontCreateFontFace(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr face_out = m.arg_ptr(1);
    if (face_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(face_out, 0);
    const auto font = m_objects.find(self);
    if (font == m_objects.end() || font->second.type != ObjectType::Font) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const u32 simulations = font->second.simulations;
    const GuestAddr face = create_object(
        ObjectType::FontFace, font->second.family_index, font->second.font);
    if (face == 0 || m.write_ptr(face_out, face).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m_objects.at(face).family_fonts = font->second.family_fonts;
    m_objects.at(face).simulations = simulations;
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateFontFace font=0x%llx -> S_OK "
                     "face=0x%llx\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(face));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetType(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    m.set_return(face != m_objects.end() &&
                         face->second.type == ObjectType::FontFace
                     ? face->second.font.face_type
                     : 6u);
}

void DWriteManager::impl_FaceGetFiles(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    const GuestAddr count_address = m.arg_ptr(1);
    const GuestAddr files_address = m.arg_ptr(2);
    if (face == m_objects.end() ||
        face->second.type != ObjectType::FontFace || count_address == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (files_address == 0) {
        m.set_return(m.write_u32(count_address, 1).is_ok() ? S_OK : E_POINTER);
        return;
    }
    const auto capacity = m.read_u32(count_address);
    if (capacity.is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    if (capacity.value() < 1) {
        (void)m.write_u32(count_address, 1);
        m.set_return(E_INVALIDARG);
        return;
    }
    auto path = publish_font_file(face->second.font);
    if (!path) {
        (void)m.write_u32(count_address, 0);
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }
    const GuestAddr file = create_local_font_file(*path);
    if (file == 0 || !write_pointer(files_address, file) ||
        m.write_u32(count_address, 1).is_err()) {
        if (file != 0) m_objects.erase(file);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (dwrite_diag_enabled()) {
        const std::string utf8 = utf16_to_utf8(
            reinterpret_cast<const u16*>(path->data()),
            static_cast<int>(path->size()));
        dwrite_diag_printf(
                     "[dwrite-diag] FontFace::GetFiles face=0x%llx "
                     "-> S_OK file=0x%llx path='%s'\n",
                     static_cast<unsigned long long>(m.arg_ptr(0)),
                     static_cast<unsigned long long>(file), utf8.c_str());
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetIndex(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    m.set_return(face != m_objects.end() &&
                         face->second.type == ObjectType::FontFace
                     ? face->second.font.face_index
                     : 0u);
}

void DWriteManager::impl_FaceGetSimulations(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    m.set_return(face != m_objects.end() &&
                         face->second.type == ObjectType::FontFace
                     ? face->second.simulations
                     : 0u);
}

void DWriteManager::impl_FaceIsSymbolFont(Marshaler& m) {
    (void)m.arg_ptr(0);
    m.set_return(0);
}

void DWriteManager::impl_FaceGetMetrics(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    const GuestAddr out = m.arg_ptr(1);
    if (face == m_objects.end() ||
        face->second.type != ObjectType::FontFace || out == 0) {
        return;
    }
    const auto& metrics = face->second.font.metrics;
    const std::array<u16, 10> fields{
        metrics.design_units_per_em,
        metrics.ascent,
        metrics.descent,
        static_cast<u16>(metrics.line_gap),
        metrics.cap_height,
        metrics.x_height,
        static_cast<u16>(metrics.underline_position),
        metrics.underline_thickness,
        static_cast<u16>(metrics.strikethrough_position),
        metrics.strikethrough_thickness,
    };
    for (usize i = 0; i < fields.size(); ++i) {
        (void)m.write_u16(out + i * sizeof(u16), fields[i]);
    }
}

void DWriteManager::impl_FaceGetGlyphCount(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    m.set_return(face != m_objects.end() &&
                         face->second.type == ObjectType::FontFace
                     ? face->second.font.glyph_count
                     : 0u);
}

void DWriteManager::impl_FaceGetWeight(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    m.set_return(face != m_objects.end() &&
                         face->second.type == ObjectType::FontFace
                     ? reported_weight(face->second.font, face->second.simulations)
                     : 0);
}

void DWriteManager::impl_FaceGetStretch(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    m.set_return(face != m_objects.end() &&
                         face->second.type == ObjectType::FontFace
                     ? face->second.font.stretch
                     : 0);
}

void DWriteManager::impl_FaceGetStyle(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    m.set_return(face != m_objects.end() &&
                         face->second.type == ObjectType::FontFace
                     ? reported_style(face->second.font, face->second.simulations)
                     : 0);
}

void DWriteManager::impl_FaceGetPanose(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return;
    for (usize i = 0; i < 10; ++i) {
        (void)m.write_u8(out + i, 0);
    }
}

void DWriteManager::impl_FaceGetFamilyNames(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr names_out = m.arg_ptr(1);
    if (names_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(names_out, 0);
    const auto face = m_objects.find(self);
    if (face == m_objects.end() || face->second.type != ObjectType::FontFace) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr names = create_object(
        ObjectType::LocalizedStrings, face->second.family_index);
    if (names == 0 || m.write_ptr(names_out, names).is_err()) {
        if (names != 0) m_objects.erase(names);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m_objects.at(names).localized_value = face->second.font.family_name;
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetFaceNames(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr names_out = m.arg_ptr(1);
    if (names_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(names_out, 0);
    const auto face = m_objects.find(self);
    if (face == m_objects.end() || face->second.type != ObjectType::FontFace) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr names = create_object(
        ObjectType::LocalizedStrings, face->second.family_index);
    if (names == 0 || m.write_ptr(names_out, names).is_err()) {
        if (names != 0) m_objects.erase(names);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m_objects.at(names).localized_value =
        face_display_name(face->second.font, face->second.simulations);
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetFamilyNamesLocale(Marshaler& m) {
    // IDWriteFontFace6::GetFamilyNames(localeNames, names) — locale ignored.
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr names_out = m.arg_ptr(2);
    if (names_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    // Reuse Face3 path by temporarily presenting names at arg1 via a local
    // marshaler-compatible call pattern: write through names_out directly.
    (void)m.write_ptr(names_out, 0);
    const auto face = m_objects.find(self);
    if (face == m_objects.end() || face->second.type != ObjectType::FontFace) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr names = create_object(
        ObjectType::LocalizedStrings, face->second.family_index);
    if (names == 0 || m.write_ptr(names_out, names).is_err()) {
        if (names != 0) m_objects.erase(names);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m_objects.at(names).localized_value = face->second.font.family_name;
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetFaceNamesLocale(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr names_out = m.arg_ptr(2);
    if (names_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(names_out, 0);
    const auto face = m_objects.find(self);
    if (face == m_objects.end() || face->second.type != ObjectType::FontFace) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr names = create_object(
        ObjectType::LocalizedStrings, face->second.family_index);
    if (names == 0 || m.write_ptr(names_out, names).is_err()) {
        if (names != 0) m_objects.erase(names);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m_objects.at(names).localized_value =
        face_display_name(face->second.font, face->second.simulations);
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetInformationalStrings(Marshaler& m) {
    const GuestAddr strings_out = m.arg_ptr(2);
    const GuestAddr exists_out = m.arg_ptr(3);
    if (strings_out == 0 || exists_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(strings_out, 0);
    if (m.write_u32(exists_out, 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceHasCharacter(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    const u32 codepoint = static_cast<u32>(m.arg(1));
    if (face == m_objects.end() || face->second.type != ObjectType::FontFace) {
        m.set_return(0);
        return;
    }
    const std::vector<u32> codepoints{codepoint};
    auto glyphs = dwrite::glyph_indices(face->second.font, codepoints);
    m.set_return(glyphs && !glyphs->empty() && (*glyphs)[0] != 0 ? 1u : 0u);
}

void DWriteManager::impl_FaceIsLocal(Marshaler& m) {
    (void)m.arg_ptr(0);
    // Installed system/custom faces are always local in the capsule.
    m.set_return(1);
}

void DWriteManager::impl_FaceAreLocal(Marshaler& m) {
    const GuestAddr is_local_out = m.arg_ptr(4);
    if (is_local_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (m.write_u32(is_local_out, 1).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceEquals(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr other = m.arg_ptr(1);
    if (self == other) {
        m.set_return(1);
        return;
    }
    const auto left = m_objects.find(self);
    const auto right = m_objects.find(other);
    if (left == m_objects.end() || right == m_objects.end() ||
        left->second.type != ObjectType::FontFace ||
        right->second.type != ObjectType::FontFace) {
        m.set_return(0);
        return;
    }
    const auto& a = left->second.font;
    const auto& b = right->second.font;
    m.set_return(
        a.family_name == b.family_name &&
                a.postscript_name == b.postscript_name &&
                a.weight == b.weight && a.stretch == b.stretch &&
                a.style == b.style
            ? 1u
            : 0u);
}

} // namespace vitreo::winapi
