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

// LOGFONTW (wingdi.h): five LONGs, eight BYTEs and WCHAR lfFaceName[32], the
// same 92 bytes on x86 and x64.
constexpr usize LOGFONTW_SIZE = 92;
constexpr usize LF_WEIGHT_OFFSET = 16;
constexpr usize LF_ITALIC_OFFSET = 20;
constexpr usize LF_CHARSET_OFFSET = 23;
constexpr usize LF_OUT_PRECISION_OFFSET = 24;
constexpr usize LF_FACE_NAME_OFFSET = 28;
constexpr usize LF_FACESIZE = 32;
constexpr u8 DEFAULT_CHARSET = 1;
constexpr u8 OUT_OUTLINE_PRECIS = 8;
// dwrite_gdi_interop_probe: CreateFontFromLOGFONT emboldens a face when
// lfWeight exceeds its weight by more than 150 (Segoe UI 550 -> Regular,
// 551 -> Regular + bold; 850 -> Bold, 851 -> Bold + bold), and
// ConvertFontToLOGFONT reports such a font 151 heavier than its face.
constexpr i32 GDI_BOLD_SIMULATION_STEP = 151;

u32 sfnt_u16(const std::vector<u8>& table, usize offset) {
    return offset + 2 <= table.size()
        ? (static_cast<u32>(table[offset]) << 8) | table[offset + 1] : 0u;
}

std::vector<u8> font_table_or_empty(const dwrite::MatchedFont& font,
                                    char a, char b, char c, char d) {
    // DWRITE_MAKE_OPENTYPE_TAG order: the first character in the low byte.
    const u32 tag = static_cast<u32>(static_cast<u8>(a)) |
        (static_cast<u32>(static_cast<u8>(b)) << 8) |
        (static_cast<u32>(static_cast<u8>(c)) << 16) |
        (static_cast<u32>(static_cast<u8>(d)) << 24);
    auto table = dwrite::font_table(font, tag);
    return table ? std::move(*table) : std::vector<u8>{};
}

/// The OpenType 'name' string @p name_id: Windows-platform US English first,
/// then any Windows or Unicode-platform record, then an ASCII Macintosh one.
std::u16string opentype_name(const std::vector<u8>& table, u32 name_id) {
    if (table.size() < 6) return {};
    const u32 count = sfnt_u16(table, 2);
    const usize storage = sfnt_u16(table, 4);
    int best_rank = 0;
    std::u16string best;
    for (u32 index = 0; index < count; ++index) {
        const usize record = 6 + static_cast<usize>(index) * 12;
        if (record + 12 > table.size()) break;
        const u32 platform = sfnt_u16(table, record);
        const u32 language = sfnt_u16(table, record + 4);
        const usize length = sfnt_u16(table, record + 8);
        const usize offset = storage + sfnt_u16(table, record + 10);
        if (sfnt_u16(table, record + 6) != name_id || length == 0 ||
            offset + length > table.size()) {
            continue;
        }
        const int rank = platform == 3 ? (language == 0x0409 ? 4 : 3)
                       : platform == 0 ? 2
                       : platform == 1 ? 1
                                       : 0;
        if (rank <= best_rank) continue;
        std::u16string value;
        if (platform == 1) {
            bool ascii = true;
            for (usize i = 0; i < length; ++i) {
                ascii = ascii && table[offset + i] < 0x80;
                value.push_back(static_cast<char16_t>(table[offset + i]));
            }
            if (!ascii) continue;
        } else {
            for (usize i = 0; i + 1 < length; i += 2) {
                value.push_back(
                    static_cast<char16_t>(sfnt_u16(table, offset + i)));
            }
        }
        best_rank = rank;
        best = std::move(value);
    }
    return best;
}

/// OS/2 usWeightClass, the weight GDI matches and reports.
u32 gdi_weight_class(const dwrite::MatchedFont& font) {
    // The host UI font is variable: its OS/2 table describes the default
    // instance whichever weight was resolved.
    if (font.system_ui_font) return font.weight;
    const auto os2 = font_table_or_empty(font, 'O', 'S', '/', '2');
    const u32 weight = sfnt_u16(os2, 4);
    return weight >= 1 && weight <= 1000 ? weight : font.weight;
}

/// The name GDI knows a face's family by: its win32 family (name ID 1).
std::u16string gdi_family_name(const dwrite::MatchedFont& font) {
    if (font.system_ui_font) {
        // The host UI font answers as Segoe UI, the Windows UI family that
        // USER32, GDI and DirectWrite already name for it; its own name is a
        // dot-prefixed private CoreText one.
        if (font.family_name.empty() || font.family_name.front() == u'.') {
            return u"Segoe UI";
        }
        return font.family_name;
    }
    auto name = opentype_name(
        font_table_or_empty(font, 'n', 'a', 'm', 'e'), 1);
    return name.empty() ? font.family_name : name;
}

} // namespace

void DWriteManager::impl_GetGdiInterop(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr interop_out = m.arg_ptr(1);
    if (interop_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(interop_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr interop = create_object(ObjectType::GdiInterop);
    if (interop == 0 || m.write_ptr(interop_out, interop).is_err()) {
        if (interop != 0) m_objects.erase(interop);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] Factory::GetGdiInterop "
                     "factory=0x%llx -> S_OK interop=0x%llx\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(interop));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_GdiCreateFontFaceFromHdc(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const Handle hdc = static_cast<Handle>(m.arg(1));
    const GuestAddr face_out = m.arg_ptr(2);
    if (face_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(face_out, 0);
    const auto interop = m_objects.find(self);
    if (interop == m_objects.end() ||
        interop->second.type != ObjectType::GdiInterop || m_gdi == nullptr) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GdiManager::FontInfo* selected = m_gdi->get_dc_font_info(hdc);
    if (selected == nullptr) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto matched = dwrite::match_system_ui_font(
        selected->weight, 5, selected->italic ? 2 : 0);
    if (!matched) {
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }
    const GuestAddr face = create_object(
        ObjectType::FontFace, 0, std::move(*matched));
    if (face == 0 || m.write_ptr(face_out, face).is_err()) {
        if (face != 0) m_objects.erase(face);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] GdiInterop::CreateFontFaceFromHdc "
                     "interop=0x%llx hdc=0x%x -> S_OK face=0x%llx\n",
                     static_cast<unsigned long long>(self), hdc,
                     static_cast<unsigned long long>(face));
    }
    m.set_return(S_OK);
}

// ---- IDWriteGdiInterop LOGFONT conversions ---------------------------------
//
// Learn gives only the signatures; tools/oracle_probe/dwrite_gdi_interop_probe
// measured the rest on Windows 11 (x64 and x86 identical):
// - lfFaceName names a GDI (win32) family or a full name, case-insensitively,
//   with no trimming, no FontSubstitutes alias ("MS Shell Dlg", "Helv"), no
//   raster family ("System", "Courier") and no GDI-private font
//   (AddFontResourceEx FR_PRIVATE): anything else is DWRITE_E_NOFONT.
// - Only lfFaceName, lfWeight and lfItalic (any non-zero value) matter.
// - A missing italic is simulated (DWRITE_FONT_STYLE_OBLIQUE); a weight more
//   than 150 above the chosen face emboldens it (GDI_BOLD_SIMULATION_STEP).
// - The Convert* methods zero the whole LOGFONTW, then write lfWeight,
//   lfItalic, lfCharSet DEFAULT_CHARSET, lfOutPrecision OUT_OUTLINE_PRECIS and
//   the win32 family name; isSystemFont is FALSE for a custom collection.

std::shared_ptr<const DWriteManager::GdiFamily> DWriteManager::gdi_family(
    const std::u16string& family) {
    const std::u16string key = fold_ascii(family);
    if (const auto cached = m_gdi_families.find(key);
        cached != m_gdi_families.end()) {
        return cached->second;
    }
    auto fonts = dwrite::enumerate_system_family_fonts(family);
    if (fonts.empty()) {
        // A family CoreText cannot enumerate (Vitreo's Segoe UI, which stands
        // in for the host UI font) is its four GDI styles.
        for (const u32 weight : {400u, 700u}) {
            for (const u32 style : {0u, 2u}) {
                auto matched = dwrite::match_system_font(family, weight, 5, style);
                if (!matched || fold_ascii(matched->family_name) != key) continue;
                if (matched->system_ui_font) {
                    // Windows' names for the four styles; the host UI font
                    // calls its italic "Regular Italic".
                    const bool bold = matched->weight >= 600;
                    const bool italic = matched->style != 0;
                    matched->face_name = bold && italic ? u"Bold Italic"
                                       : bold          ? u"Bold"
                                       : italic        ? u"Italic"
                                                       : u"Regular";
                }
                const bool duplicate = std::any_of(
                    fonts.begin(), fonts.end(), [&](const auto& font) {
                        return font.postscript_name == matched->postscript_name &&
                               font.weight == matched->weight &&
                               font.style == matched->style;
                    });
                if (!duplicate) fonts.push_back(std::move(*matched));
            }
        }
    }
    auto result = std::make_shared<GdiFamily>();
    result->fonts = std::make_shared<std::vector<dwrite::MatchedFont>>(fonts);
    for (auto& font : fonts) {
        GdiFace face;
        face.weight = font.weight;
        if (!font.system_ui_font) {
            const auto names = font_table_or_empty(font, 'n', 'a', 'm', 'e');
            face.win32_family = opentype_name(names, 1);
            face.full_name = opentype_name(names, 4);
            face.weight = gdi_weight_class(font);
        }
        if (face.win32_family.empty()) face.win32_family = font.family_name;
        if (face.full_name.empty()) {
            face.full_name = font.family_name;
            if (!font.face_name.empty() && fold_ascii(font.face_name) != u"regular") {
                face.full_name += u' ';
                face.full_name += font.face_name;
            }
        }
        face.font = std::move(font);
        result->faces.push_back(std::move(face));
    }
    m_gdi_families.emplace(key, result);
    return result;
}

bool DWriteManager::write_gdi_logfont(GuestAddr address, const Object& object) {
    std::array<u8, LOGFONTW_SIZE> logfont{};
    const bool bold = (object.simulations & DWRITE_FONT_SIMULATIONS_BOLD) != 0;
    const i32 weight = static_cast<i32>(gdi_weight_class(object.font)) +
                       (bold ? GDI_BOLD_SIMULATION_STEP : 0);
    std::memcpy(logfont.data() + LF_WEIGHT_OFFSET, &weight, sizeof(weight));
    logfont[LF_ITALIC_OFFSET] =
        reported_style(object.font, object.simulations) != 0 ? 1 : 0;
    logfont[LF_CHARSET_OFFSET] = DEFAULT_CHARSET;
    logfont[LF_OUT_PRECISION_OFFSET] = OUT_OUTLINE_PRECIS;
    const std::u16string name = gdi_family_name(object.font);
    const usize length = std::min(name.size(), LF_FACESIZE - 1);
    for (usize index = 0; index < length; ++index) {
        const u16 unit = static_cast<u16>(name[index]);
        std::memcpy(logfont.data() + LF_FACE_NAME_OFFSET + index * sizeof(u16),
                    &unit, sizeof(unit));
    }
    return m_memory.write_bytes(address, reinterpret_cast<const Byte*>(logfont.data()),
                                logfont.size()).is_ok();
}

void DWriteManager::impl_GdiCreateFontFromLogfont(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr logfont_address = m.arg_ptr(1);
    const GuestAddr font_out = m.arg_ptr(2);
    // The probe's (logFont, NULL) and (NULL, NULL) calls fault inside
    // dwrite.dll: *font is written before anything is checked.
    if (font_out == 0 || m.write_ptr(font_out, 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    const auto interop = m_objects.find(self);
    if (logfont_address == 0 || interop == m_objects.end() ||
        interop->second.type != ObjectType::GdiInterop) {
        m.set_return(E_INVALIDARG);
        return;
    }
    std::array<u8, LOGFONTW_SIZE> logfont{};
    if (m_memory.read_bytes(logfont_address, reinterpret_cast<Byte*>(logfont.data()),
                            logfont.size()).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    i32 requested_weight = 0;
    std::memcpy(&requested_weight, logfont.data() + LF_WEIGHT_OFFSET,
                sizeof(requested_weight));
    // FW_DONTCARE (and the probe's -1) select the Regular face.
    if (requested_weight <= 0) requested_weight = 400;
    const bool want_italic = logfont[LF_ITALIC_OFFSET] != 0;
    std::u16string requested;
    for (usize index = 0; index < LF_FACESIZE; ++index) {
        u16 unit = 0;
        std::memcpy(&unit, logfont.data() + LF_FACE_NAME_OFFSET + index * sizeof(u16),
                    sizeof(unit));
        if (unit == 0) break;
        requested.push_back(static_cast<char16_t>(unit));
    }
    const std::u16string folded = fold_ascii(requested);

    // A GDI family name selects that family's faces; a full name ("Arial
    // Bold") selects one face of the family its leading words name.
    std::u16string family_name;
    std::shared_ptr<const GdiFamily> family;
    std::vector<const GdiFace*> candidates;
    const auto find_family = [&](std::u16string_view name) {
        const auto found = std::find_if(
            m_font_families.begin(), m_font_families.end(),
            [&](const std::u16string& known) { return fold_ascii(known) == name; });
        return found == m_font_families.end() ? std::u16string{} : *found;
    };
    if (!folded.empty()) {
        if (auto name = find_family(folded); !name.empty()) {
            family_name = std::move(name);
            family = gdi_family(family_name);
            for (const auto& face : family->faces) {
                if (fold_ascii(face.win32_family) == folded) candidates.push_back(&face);
            }
            // A CoreText family whose faces carry other win32 names still
            // answers under its own name.
            if (candidates.empty()) {
                for (const auto& face : family->faces) candidates.push_back(&face);
            }
        }
        for (usize space = folded.rfind(u' '); candidates.empty() &&
                                               space != std::u16string::npos && space > 0;
             space = folded.rfind(u' ', space - 1)) {
            auto name = find_family(std::u16string_view(folded).substr(0, space));
            if (name.empty()) continue;
            auto faces = gdi_family(name);
            for (const auto& face : faces->faces) {
                if (fold_ascii(face.full_name) == folded ||
                    fold_ascii(face.win32_family) == folded) {
                    candidates.push_back(&face);
                }
            }
            if (!candidates.empty()) {
                family_name = std::move(name);
                family = std::move(faces);
            }
        }
    }
    if (candidates.empty()) {
        if (folded.rfind(u"segoe ui ", 0) == 0) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED dwrite!IDWriteGdiInterop::"
                             "CreateFontFromLOGFONT: the Segoe UI stand-in has no "
                             "faces beyond Regular/Bold/Italic (\"%s\" -> "
                             "DWRITE_E_NOFONT)\n",
                             utf16_to_utf8(reinterpret_cast<const u16*>(requested.data()),
                                           static_cast<int>(requested.size())).c_str());
            }
        }
        m.set_return(DWRITE_E_NOFONT);
        return;
    }

    // Italic before weight: the upright/italic faces the request wants, or
    // all of them when the family has none (then simulated oblique).
    const auto is_italic = [](const GdiFace* face) { return face->font.style != 0; };
    std::vector<const GdiFace*> styled;
    std::copy_if(candidates.begin(), candidates.end(), std::back_inserter(styled),
                 [&](const GdiFace* face) { return is_italic(face) == want_italic; });
    if (styled.empty()) styled = candidates;
    // The nearest weight, counting an emboldened face as 151 heavier; the
    // lighter face wins a tie (probe: Segoe UI 550 -> Regular, 649 -> Bold).
    const auto effective_weight = [&](const GdiFace* face) {
        const i32 weight = static_cast<i32>(face->weight);
        return requested_weight - weight > GDI_BOLD_SIMULATION_STEP - 1
            ? weight + GDI_BOLD_SIMULATION_STEP : weight;
    };
    const GdiFace* chosen = nullptr;
    i32 best_distance = 0;
    for (const GdiFace* face : styled) {
        const i32 distance = std::abs(requested_weight - effective_weight(face));
        if (chosen == nullptr || distance < best_distance ||
            (distance == best_distance && face->weight < chosen->weight)) {
            chosen = face;
            best_distance = distance;
        }
    }
    u32 simulations = 0;
    if (effective_weight(chosen) != static_cast<i32>(chosen->weight)) {
        simulations |= DWRITE_FONT_SIMULATIONS_BOLD;
    }
    if (want_italic && !is_italic(chosen)) simulations |= DWRITE_FONT_SIMULATIONS_OBLIQUE;

    const usize family_index = ensure_family_index(family_name);
    const GuestAddr font = create_object(ObjectType::Font, family_index, chosen->font);
    if (font == 0 || m.write_ptr(font_out, font).is_err()) {
        if (font != 0) m_objects.erase(font);
        (void)m.write_ptr(font_out, 0);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    auto& object = m_objects.at(font);
    object.simulations = simulations;
    object.family_fonts = family->fonts;
    if (simulations != 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED dwrite!IDWriteGdiInterop::"
                         "CreateFontFromLOGFONT: DWRITE_FONT_SIMULATIONS 0x%x is "
                         "reported but glyphs are rasterized without it\n",
                         simulations);
        }
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
            "[dwrite-diag] GdiInterop::CreateFontFromLOGFONT weight=%d italic=%u "
            "-> S_OK font=0x%llx simulations=%u\n",
            requested_weight, want_italic ? 1u : 0u,
            static_cast<unsigned long long>(font), simulations);
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_GdiConvertFontToLogfont(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr font_address = m.arg_ptr(1);
    const GuestAddr logfont = m.arg_ptr(2);
    const GuestAddr is_system_font = m.arg_ptr(3);
    // Probe: the LOGFONTW is zeroed and *isSystemFont cleared before the font
    // is examined (a NULL font answers E_INVALIDARG with both written); a
    // NULL logFont or isSystemFont faults inside dwrite.dll.
    const std::array<u8, LOGFONTW_SIZE> zero{};
    if (logfont == 0 || is_system_font == 0 ||
        m_memory.write_bytes(logfont, reinterpret_cast<const Byte*>(zero.data()),
                             zero.size()).is_err() ||
        m.write_u32(is_system_font, 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    const auto interop = m_objects.find(self);
    const auto font = m_objects.find(font_address);
    if (interop == m_objects.end() || interop->second.type != ObjectType::GdiInterop ||
        font == m_objects.end() || font->second.type != ObjectType::Font) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (!write_gdi_logfont(logfont, font->second)) {
        m.set_return(E_POINTER);
        return;
    }
    // Fonts parsed from caller bytes belong to a custom collection (probe:
    // the same arial.ttf through CreateCustomFontCollection answers FALSE).
    (void)m.write_u32(is_system_font, font->second.font.source_data ? 0u : 1u);
    m.set_return(S_OK);
}

void DWriteManager::impl_GdiConvertFontFaceToLogfont(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr face_address = m.arg_ptr(1);
    const GuestAddr logfont = m.arg_ptr(2);
    // Probe: zeroed first, as ConvertFontToLOGFONT; a NULL logFont faults.
    const std::array<u8, LOGFONTW_SIZE> zero{};
    if (logfont == 0 ||
        m_memory.write_bytes(logfont, reinterpret_cast<const Byte*>(zero.data()),
                             zero.size()).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    const auto interop = m_objects.find(self);
    const auto face = m_objects.find(face_address);
    if (interop == m_objects.end() || interop->second.type != ObjectType::GdiInterop ||
        face == m_objects.end() || face->second.type != ObjectType::FontFace) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(write_gdi_logfont(logfont, face->second) ? S_OK : E_POINTER);
}

} // namespace vitreo::winapi
