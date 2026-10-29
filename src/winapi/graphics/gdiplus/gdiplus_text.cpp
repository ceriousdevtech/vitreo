// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_text.cpp
/// @brief GDI+ area: font families, fonts, font collections and string
/// formats (the text drawing/measuring exports are in
/// gdiplus_text_layout.cpp).
///
/// Every value Learn leaves open comes from tools/oracle_probe/
/// gdiplus_text_probe.c run on Windows 11 (x86 and x64 identical):
///  - family lookup: case-insensitive, through GDI's FontSubstitutes
///    ("MS Shell Dlg" -> Microsoft Sans Serif, reported under the
///    substitute's name); raster-only and unknown names fail with
///    FontFamilyNotFound and a NULL *fontFamily;
///  - families are shared per collection (create twice / clone / list all
///    return one pointer; delete never invalidates it);
///  - the generic families are Microsoft Sans Serif / Times New Roman /
///    Courier New; IsStyleAvailable is TRUE for every style of a TrueType
///    family (simulation covers missing members);
///  - CreateFont: emSize > 0 and unit in {World, Pixel..Millimeter}; any
///    style integer is stored as given;
///  - GetFontHeight(NULL graphics) is the line spacing in the font's unit
///    for World/Pixel fonts (it faults on Windows for the others, so they
///    are InvalidParameter here);
///  - LOGFONT round trips: lfHeight = -round(em in device pixels), lfWeight
///    400/700, italic/underline/strikeout from the style bits, every other
///    field 0; FromLogfont/FromDC give a UnitWorld font of the realised em
///    (negative lfHeight as given, 0 = 16, positive = GDI's cell mapping),
///    bold from lfWeight >= 600, NotTrueTypeFont (and NULL *font) for a face
///    GDI+ cannot name;
///  - string formats: created ones keep any flags value and start with
///    StringTrimmingCharacter; the generic default/typographic formats are
///    shared, mutable, never deleted.

#include "winapi/graphics/gdiplus/gdiplus_text.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_graphics.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace vitreo::winapi::gdip {

namespace {

std::u16string fold(std::u16string_view s) {
    std::u16string out(s);
    for (auto& c : out) {
        if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c + 32);
    }
    return out;
}

u32 be16(const std::vector<u8>& t, usize o) {
    return o + 2 <= t.size() ? (static_cast<u32>(t[o]) << 8) | t[o + 1] : 0u;
}
i32 bes16(const std::vector<u8>& t, usize o) { return static_cast<i16>(be16(t, o)); }

constexpr u32 tag(char a, char b, char c, char d) {
    // DWRITE_MAKE_OPENTYPE_TAG byte order: the first character is the low byte.
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}

std::vector<u8> table_of(const dwrite::MatchedFont& font, u32 id) {
    auto bytes = dwrite::font_table(font, id);
    return bytes ? std::move(*bytes) : std::vector<u8>{};
}

}  // namespace

std::shared_ptr<const TextFace> text_face_from(dwrite::MatchedFont font, i32 style) {
    auto face = std::make_shared<TextFace>();
    const auto head = table_of(font, tag('h', 'e', 'a', 'd'));
    const auto hhea = table_of(font, tag('h', 'h', 'e', 'a'));
    const auto os2 = table_of(font, tag('O', 'S', '/', '2'));
    const auto post = table_of(font, tag('p', 'o', 's', 't'));
    face->upem = head.size() >= 20 && be16(head, 18) != 0 ? be16(head, 18)
                 : font.metrics.design_units_per_em != 0 ? font.metrics.design_units_per_em
                                                         : 2048u;
    i32 hhea_ascent = font.metrics.ascent, hhea_descent = -static_cast<i32>(font.metrics.descent);
    i32 hhea_gap = font.metrics.line_gap;
    if (hhea.size() >= 10) {
        hhea_ascent = bes16(hhea, 4);
        hhea_descent = bes16(hhea, 6);
        hhea_gap = bes16(hhea, 8);
    }
    if (os2.size() >= 78) {
        face->win_ascent = be16(os2, 74);
        face->win_descent = be16(os2, 76);
        face->strikeout_size = bes16(os2, 26);
        face->strikeout_position = bes16(os2, 28);
    } else {
        face->win_ascent = static_cast<u32>(std::max(0, hhea_ascent));
        face->win_descent = static_cast<u32>(std::max(0, -hhea_descent));
        face->strikeout_size = static_cast<i32>(face->upem / 20);
        face->strikeout_position = static_cast<i32>(face->upem / 4);
    }
    // Line spacing: the Windows cell plus whatever external leading the
    // hhea metrics add beyond it (Arial 1854 + 434 + 67 = 2355; Courier New
    // 2320 with none; gdiplus_text_probe "family ...").
    const i32 cell = static_cast<i32>(face->win_ascent + face->win_descent);
    const i32 hhea_line = hhea_ascent - hhea_descent + hhea_gap;
    face->line_spacing = static_cast<u32>(cell + std::max(0, hhea_line - cell));
    if (post.size() >= 12) {
        face->underline_position = bes16(post, 8);
        face->underline_thickness = bes16(post, 10);
    } else {
        face->underline_position = font.metrics.underline_position;
        face->underline_thickness = font.metrics.underline_thickness;
    }
    if (face->underline_thickness <= 0) face->underline_thickness = static_cast<i32>(face->upem / 14);
    if (face->strikeout_size <= 0) face->strikeout_size = face->underline_thickness;
    // 'kern' table, version 0, horizontal format-0 subtables.
    const auto kern = table_of(font, tag('k', 'e', 'r', 'n'));
    if (kern.size() >= 4 && be16(kern, 0) == 0) {
        usize off = 4;
        for (u32 t = 0, n = be16(kern, 2); t < n && off + 6 <= kern.size(); ++t) {
            const u32 length = be16(kern, off + 2), coverage = be16(kern, off + 4);
            if ((coverage >> 8) == 0 && (coverage & 1u) != 0 && (coverage & 4u) == 0 && off + 14 <= kern.size()) {
                const u32 pairs = be16(kern, off + 6);
                for (u32 k = 0; k < pairs && off + 14 + k * 6 + 6 <= kern.size(); ++k) {
                    const usize e = off + 14 + k * 6;
                    face->kerning[(be16(kern, e) << 16) | be16(kern, e + 2)] = static_cast<i16>(bes16(kern, e + 4));
                }
            }
            if (length < 6) break;
            off += length;
        }
    }
    face->simulated_bold = (style & FontStyleBold) != 0 && font.weight < 600;
    face->simulated_italic = (style & FontStyleItalic) != 0 && font.style == 0;
    face->font = std::move(font);
    return face;
}

namespace {

/// Per-manager text state: the shared objects GDI+ hands out repeatedly.
struct TextState {
    std::unordered_map<std::u16string, u64> installed_families;  ///< folded name -> family
    u64 generic_families[3]{0, 0, 0};  ///< sans serif, serif, monospace
    u64 installed_collection{0};
    u64 generic_default{0};
    u64 generic_typographic{0};
};

TextState& text_state(GdiplusManager& mgr) {
    static std::mutex mutex;
    static std::unordered_map<const GdiplusManager*, u64> handles;
    std::lock_guard<std::mutex> lock(mutex);
    u64& h = handles[&mgr];
    if (TextState* s = mgr.objects().get<TextState>(h)) return *s;
    h = mgr.objects().add(TextState{});
    return *mgr.objects().get<TextState>(h);
}

/// The installed family @p name names (FontSubstitutes applied), its
/// shared handle; 0 when the host has no such family.
u64 installed_family(GdiplusManager& mgr, const std::u16string& name) {
    if (name.empty()) return 0;
    // Windows' raster/vector .fon families have no TrueType file, so GDI+
    // cannot use them (probe "family 'Courier'/'System'/'MS Sans Serif'"),
    // even where the host has a family of that name (macOS Courier). The
    // same list as GDI's mapper (text_engine.cpp raster_font_replacement).
    {
        const std::u16string f = fold(name);
        for (const char16_t* raster : {u"system", u"ms sans serif", u"small fonts", u"terminal", u"fixedsys",
                                       u"courier", u"modern", u"ms serif", u"roman", u"script"}) {
            if (f == raster) return 0;
        }
    }
    const std::u16string alias = GdiManager::substitute_face(name);
    TextState& state = text_state(mgr);
    if (auto it = state.installed_families.find(fold(alias)); it != state.installed_families.end()) {
        return it->second;
    }
    // Core Text answers an unknown name with a default face; only an exact
    // family match is that family (as GDI's mapper, text_engine.cpp).
    auto matched = dwrite::match_system_font(alias, 400, 5, 0);
    if (!matched || fold(matched->family_name) != fold(alias)) return 0;
    auto family = std::make_shared<GpFontFamily>();
    family->name = matched->family_name;
    family->faces[0] = text_face_from(std::move(*matched), 0);
    const u64 h = mgr.objects().add(family);
    state.installed_families[fold(alias)] = h;
    state.installed_families[fold(family->name)] = h;
    return h;
}

/// A family the installed list names (no validation round trip: the host
/// enumerated it).
u64 listed_family(GdiplusManager& mgr, const std::u16string& name) {
    TextState& state = text_state(mgr);
    if (auto it = state.installed_families.find(fold(name)); it != state.installed_families.end()) {
        return it->second;
    }
    auto family = std::make_shared<GpFontFamily>();
    family->name = name;
    const u64 h = mgr.objects().add(family);
    state.installed_families[fold(name)] = h;
    return h;
}

std::vector<std::u16string>& installed_names() {
    static std::vector<std::u16string> names = [] {
        auto list = dwrite::enumerate_system_font_families();
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return fold(a) < fold(b); });
        list.erase(std::unique(list.begin(), list.end(),
                               [](const auto& a, const auto& b) { return fold(a) == fold(b); }),
                   list.end());
        return list;
    }();
    return names;
}

u64 installed_collection(GdiplusManager& mgr) {
    TextState& state = text_state(mgr);
    if (mgr.objects().get<GpFontCollection>(state.installed_collection) == nullptr) {
        GpFontCollection c;
        c.installed = true;
        state.installed_collection = mgr.objects().add(std::move(c));
    }
    return state.installed_collection;
}

u64 generic_format(GdiplusManager& mgr, bool typographic) {
    TextState& state = text_state(mgr);
    u64& h = typographic ? state.generic_typographic : state.generic_default;
    if (mgr.objects().get<GpStringFormat>(h) == nullptr) {
        GpStringFormat f;
        f.generic = true;
        if (typographic) {
            // GdiPlusStringFormat.h GenericTypographic; probe "generictypographic".
            f.flags = StringFormatFlagsNoFitBlackBox | StringFormatFlagsLineLimit | StringFormatFlagsNoClip;
            f.trimming = StringTrimmingNone;
            f.typographic = true;
        }
        h = mgr.objects().add(std::move(f));
    }
    return h;
}

i32 round_half_away(float v) { return static_cast<i32>(v >= 0 ? v + 0.5f : v - 0.5f); }

bool valid_font_unit(i32 unit) { return unit == UnitWorld || (unit >= UnitPixel && unit <= UnitMillimeter); }

/// LOGFONT -> GdiManager::FontInfo (LOGFONTW: face at 28, 32 WCHARs;
/// LOGFONTA: 32 bytes of the ANSI code page). Both layouts are the same on
/// x86 and x64 (no pointer members).
bool read_logfont(Marshaler& m, GuestAddr at, bool wide, GdiManager::FontInfo& info) {
    u8 raw[92]{};
    const usize size = wide ? 92 : 60;
    if (m.memory().read_bytes(at, raw, size).is_err()) return false;
    auto i32_at = [&](usize o) { i32 v; std::memcpy(&v, raw + o, 4); return v; };
    info.height = i32_at(0);
    info.width = i32_at(4);
    info.escapement = i32_at(8);
    info.orientation = i32_at(12);
    info.lf_weight = i32_at(16);
    info.weight = static_cast<u16>(std::clamp(info.lf_weight, 0, 1000));
    info.italic = raw[20] != 0;
    info.underline = raw[21];
    info.strike_out = raw[22];
    info.char_set = raw[23];
    info.out_precision = raw[24];
    info.clip_precision = raw[25];
    info.quality = raw[26];
    info.pitch_and_family = raw[27];
    info.face_name.clear();
    if (wide) {
        for (usize i = 0; i < 32; ++i) {
            u16 c;
            std::memcpy(&c, raw + 28 + i * 2, 2);
            if (c == 0) break;
            info.face_name.push_back(static_cast<char16_t>(c));
        }
    } else {
        usize n = 0;
        while (n < 32 && raw[28 + n] != 0) ++n;
        u16 wbuf[32]{};
        const auto r = codepage::multi_byte_to_wide(codepage::kCpAcp, 0, raw + 28, n, wbuf, 32);
        for (u32 i = 0; i < r.count; ++i) info.face_name.push_back(static_cast<char16_t>(wbuf[i]));
    }
    return true;
}

/// GdipCreateFontFromLogfont/FromDC: the realised em of @p info on @p hdc.
float logfont_em(GdiManager& gdi, Handle hdc, const GdiManager::FontInfo& info) {
    if (info.height < 0) return static_cast<float>(-static_cast<i64>(info.height));
    if (info.height == 0) return 16.0f;  // GDI's default: 12 pt at 96 dpi (probe "h=0")
    // A positive height is a cell height: the em is what GDI's mapper picks
    // (VDMX), so realise it exactly as GDI does on this DC.
    const Handle font = gdi.create_font(info);
    if (font == 0) return 0;
    const Handle old = gdi.select_object(hdc, font);
    const auto face = gdi.text_face(hdc);
    const float em = face ? static_cast<float>(face->ppem) : 0.0f;
    if (old != 0) (void)gdi.select_object(hdc, old);
    (void)gdi.delete_object(font);
    return em;
}

Status font_from_logfont(GdiplusManager& mgr, Handle hdc, const GdiManager::FontInfo& info, u64& out) {
    out = 0;
    GdiManager* gdi = mgr.gdi();
    if (gdi == nullptr || !gdi->has_dc(hdc)) return Status::NotTrueTypeFont;
    const u64 family = installed_family(mgr, info.face_name);
    if (family == 0) return Status::NotTrueTypeFont;
    const float em = logfont_em(*gdi, hdc, info);
    if (!(em > 0)) return Status::NotTrueTypeFont;
    GpFont font;
    font.family = family;
    font.family_object = mgr.objects().share<GpFontFamily>(family);
    font.em_size = em;
    font.style = (info.lf_weight >= 600 ? FontStyleBold : 0) | (info.italic ? FontStyleItalic : 0) |
                 (info.underline ? FontStyleUnderline : 0) | (info.strike_out ? FontStyleStrikeout : 0);
    font.unit = UnitWorld;
    out = mgr.objects().add(std::move(font));
    return Status::Ok;
}

/// Line spacing of @p font in its own unit.
float font_line_spacing(const GpFont& font) {
    const auto face = family_face(*font.family_object, font.style);
    if (!face) return 0;
    return font.em_size * static_cast<float>(face->line_spacing) / static_cast<float>(face->upem);
}

bool write_out_u16(Marshaler& m, GuestAddr at, u32 v) { return m.write_u16(at, static_cast<u16>(v)).is_ok(); }

}  // namespace

// ---- Faces ------------------------------------------------------------------

std::shared_ptr<const TextFace> family_face(const GpFontFamily& family, i32 style) {
    const i32 index = style & (FontStyleBold | FontStyleItalic);
    if (family.faces[static_cast<usize>(index)]) return family.faces[static_cast<usize>(index)];
    const bool bold = (index & FontStyleBold) != 0, italic = (index & FontStyleItalic) != 0;
    std::optional<dwrite::MatchedFont> chosen;
    if (!family.private_faces.empty()) {
        const dwrite::MatchedFont* best = nullptr;
        i32 best_score = 0;
        for (const auto& f : family.private_faces) {
            const i32 score = std::abs(static_cast<i32>(f.weight) - (bold ? 700 : 400)) +
                              (((f.style != 0) != italic) ? 1000 : 0);
            if (best == nullptr || score < best_score) {
                best = &f;
                best_score = score;
            }
        }
        if (best != nullptr) chosen = *best;
    } else {
        auto m = dwrite::match_system_font(family.name, bold ? 700 : 400, 5, italic ? 2 : 0);
        if (m && fold(m->family_name) == fold(family.name)) chosen = std::move(m);
        if (!chosen && index != 0) {
            auto regular = family_face(family, 0);
            if (regular) chosen = regular->font;
        }
    }
    if (!chosen) return nullptr;
    auto face = text_face_from(std::move(*chosen), index);
    family.faces[static_cast<usize>(index)] = face;
    return face;
}

float font_em_world(const GpFont& font, const GpGraphics& g) {
    if (font.unit == UnitWorld) return font.em_size;
    const float device = font.em_size * unit_to_device(font.unit, g.dpi_y);
    const float page = unit_to_device(g.state.page_unit, g.dpi_y) * g.state.page_scale;
    return page != 0 ? device / page : device;
}

const GpStringFormat& implicit_default_format() {
    static const GpStringFormat format = [] {
        GpStringFormat f;
        f.implicit_default = true;
        return f;
    }();
    return format;
}

// ---- Registration -----------------------------------------------------------

void register_text(GdiplusManager& mgr, HandlerMap& map) {
    // ---- Font families ----
    map["GdipCreateFontFamilyFromName"] = [&mgr](Marshaler& m) {
        const GuestAddr name_at = m.arg_ptr(0);
        const u64 collection = handle_arg(m, 1);
        const GuestAddr out = m.arg_ptr(2);
        if (name_at == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
        const std::u16string name = read_wstr(m, name_at);
        u64 family = 0;
        if (collection != 0) {
            GpFontCollection* c = mgr.objects().get<GpFontCollection>(collection);
            if (c == nullptr) { write_status(m, Status::InvalidParameter); return; }
            if (c->installed) {
                family = installed_family(mgr, name);
            } else {
                for (const auto& f : c->families) {
                    if (fold(f.name) == fold(name)) family = f.handle;
                }
            }
        } else {
            family = installed_family(mgr, name);
        }
        write_handle(m, out, family);  // NULL on failure (probe "out=null")
        write_status(m, family != 0 ? Status::Ok : Status::FontFamilyNotFound);
    };
    map["GdipCloneFontFamily"] = [&mgr](Marshaler& m) {
        const u64 h = handle_arg(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (mgr.objects().get<GpFontFamily>(h) == nullptr || out == 0) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        // The shared family object itself (probe "clone ... same").
        write_handle(m, out, h);
        write_status(m, Status::Ok);
    };
    map["GdipDeleteFontFamily"] = [&mgr](Marshaler& m) {
        // Families belong to their collection's cache; deleting a reference
        // leaves the object valid (probe "delete st=0 0 0", generic reuse).
        write_status(m, mgr.objects().get<GpFontFamily>(handle_arg(m, 0)) ? Status::Ok
                                                                         : Status::InvalidParameter);
    };
    map["GdipGetFamilyName"] = [&mgr](Marshaler& m) {
        GpFontFamily* f = mgr.objects().get<GpFontFamily>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        // m.arg32(2): LANGID. Each family carries one name here; Windows
        // answers the same English name for 0x407/0x411 too (probe).
        if (f == nullptr) { write_status(m, Status::InvalidParameter); return; }
        if (out != 0) {  // a NULL buffer is Ok and writes nothing (probe "null-buf")
            std::u16string name = f->name.substr(0, 31);  // LF_FACESIZE incl. NUL
            write_wstr(m, out, name, true);
        }
        write_status(m, Status::Ok);
    };
    auto generic = [&mgr](HandlerMap& mp, const char* api, usize index, const char16_t* name) {
        mp[api] = [&mgr, index, name](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(0);
            if (out == 0) { write_status(m, Status::InvalidParameter); return; }
            TextState& state = text_state(mgr);
            if (mgr.objects().get<GpFontFamily>(state.generic_families[index]) == nullptr) {
                state.generic_families[index] = installed_family(mgr, name);
            }
            const u64 h = state.generic_families[index];
            if (h == 0) {
                write_handle(m, out, 0);
                write_status(m, Status::FontFamilyNotFound);
                return;
            }
            write_handle(m, out, h);
            write_status(m, Status::Ok);
        };
    };
    generic(map, "GdipGetGenericFontFamilySansSerif", 0, u"Microsoft Sans Serif");
    generic(map, "GdipGetGenericFontFamilySerif", 1, u"Times New Roman");
    generic(map, "GdipGetGenericFontFamilyMonospace", 2, u"Courier New");

    map["GdipIsStyleAvailable"] = [&mgr](Marshaler& m) {
        GpFontFamily* f = mgr.objects().get<GpFontFamily>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(2);
        if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        // GDI+ simulates bold/italic and draws underline/strikeout itself, so
        // every style of a usable family is available (probe "avail").
        const bool usable = family_face(*f, 0) != nullptr;
        (void)m.write_u32(out, usable ? 1u : 0u);
        write_status(m, Status::Ok);
    };
    auto metric = [&mgr](HandlerMap& mp, const char* api, u32 (*pick)(const TextFace&)) {
        mp[api] = [&mgr, pick](Marshaler& m) {
            GpFontFamily* f = mgr.objects().get<GpFontFamily>(handle_arg(m, 0));
            const i32 style = static_cast<i32>(m.arg32(1));
            const GuestAddr out = m.arg_ptr(2);
            if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
            const auto face = family_face(*f, style);
            if (!face) { write_status(m, Status::FontStyleNotFound); return; }
            write_out_u16(m, out, pick(*face));
            write_status(m, Status::Ok);
        };
    };
    metric(map, "GdipGetEmHeight", [](const TextFace& f) { return f.upem; });
    metric(map, "GdipGetCellAscent", [](const TextFace& f) { return f.win_ascent; });
    metric(map, "GdipGetCellDescent", [](const TextFace& f) { return f.win_descent; });
    metric(map, "GdipGetLineSpacing", [](const TextFace& f) { return f.line_spacing; });

    // ---- Font collections ----
    map["GdipNewInstalledFontCollection"] = [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        write_handle(m, out, installed_collection(mgr));  // one per process (probe "same")
        write_status(m, Status::Ok);
    };
    map["GdipNewPrivateFontCollection"] = [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        write_handle(m, out, mgr.objects().add(GpFontCollection{}));
        write_status(m, Status::Ok);
    };
    map["GdipDeletePrivateFontCollection"] = [&mgr](Marshaler& m) {
        const GuestAddr at = m.arg_ptr(0);
        if (at == 0) { write_status(m, Status::InvalidParameter); return; }
        const auto h = m.read_ptr(at);
        if (h.is_err()) { write_status(m, Status::InvalidParameter); return; }
        GpFontCollection* c = mgr.objects().get<GpFontCollection>(h.value());
        // A private collection is destroyed (its families with it); the
        // installed one only loses this reference. Either way *p = NULL; a
        // NULL *p is Ok (probe "private delete ... out=null", "ptr-to-null").
        if (c != nullptr && !c->installed) {
            for (const auto& f : c->families) (void)mgr.objects().remove<GpFontFamily>(f.handle);
            (void)mgr.objects().remove<GpFontCollection>(h.value());
        } else if (c == nullptr && h.value() != 0) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        write_handle(m, at, 0);
        write_status(m, Status::Ok);
    };
    map["GdipGetFontCollectionFamilyCount"] = [&mgr](Marshaler& m) {
        GpFontCollection* c = mgr.objects().get<GpFontCollection>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (c == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        const usize n = c->installed ? installed_names().size() : c->families.size();
        (void)m.write_u32(out, static_cast<u32>(n));
        write_status(m, Status::Ok);
    };
    map["GdipGetFontCollectionFamilyList"] = [&mgr](Marshaler& m) {
        GpFontCollection* c = mgr.objects().get<GpFontCollection>(handle_arg(m, 0));
        const i32 sought = static_cast<i32>(m.arg32(1));
        const GuestAddr list = m.arg_ptr(2);
        const GuestAddr found = m.arg_ptr(3);
        if (c == nullptr || list == 0 || found == 0) { write_status(m, Status::InvalidParameter); return; }
        const usize n = c->installed ? installed_names().size() : c->families.size();
        // numSought <= 0 is Ok with *numFound = 0 (probe "count0", "neg").
        const usize want = sought > 0 ? std::min(n, static_cast<usize>(sought)) : 0;
        const usize ptr = m.cpu_is_64bit() ? 8 : 4;
        for (usize i = 0; i < want; ++i) {
            const u64 h = c->installed ? listed_family(mgr, installed_names()[i]) : c->families[i].handle;
            write_handle(m, list + i * ptr, h);
        }
        (void)m.write_u32(found, static_cast<u32>(want));
        write_status(m, Status::Ok);
    };
    auto add_faces = [&mgr](GpFontCollection& c, std::vector<dwrite::MatchedFont> faces) {
        for (auto& face : faces) {
            const std::u16string name = face.family_name.empty() ? face.face_name : face.family_name;
            auto it = std::find_if(c.families.begin(), c.families.end(),
                                   [&](const GpFontCollection::Family& f) { return fold(f.name) == fold(name); });
            if (it == c.families.end()) {
                auto family = std::make_shared<GpFontFamily>();
                family->name = name;
                c.families.push_back({name, mgr.objects().add(family)});
                it = c.families.end() - 1;
            }
            GpFontFamily* family = mgr.objects().get<GpFontFamily>(it->handle);
            if (family == nullptr) continue;
            const bool dup = std::any_of(family->private_faces.begin(), family->private_faces.end(),
                                         [&](const dwrite::MatchedFont& f) {
                                             return f.postscript_name == face.postscript_name &&
                                                    !f.postscript_name.empty();
                                         });
            if (!dup) {
                family->private_faces.push_back(std::move(face));
                family->faces = {};
            }
        }
    };
    map["GdipPrivateAddFontFile"] = [&mgr, add_faces](Marshaler& m) {
        GpFontCollection* c = mgr.objects().get<GpFontCollection>(handle_arg(m, 0));
        const GuestAddr name_at = m.arg_ptr(1);
        if (c == nullptr || c->installed || name_at == 0) { write_status(m, Status::InvalidParameter); return; }
        const std::u16string path = read_wstr(m, name_at);
        capsule::Vfs* vfs = mgr.vfs();
        if (vfs == nullptr) { write_status(m, Status::FileNotFound); return; }
        std::string utf8;
        for (usize i = 0; i < path.size(); ++i) {
            u32 cp = path[i];
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < path.size()) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<u32>(path[++i]) - 0xDC00);
            }
            if (cp < 0x80) {
                utf8.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        auto opened = vfs->open(utf8, capsule::AccessMode::Read, capsule::ShareMode::Read,
                                capsule::CreationDisposition::OpenExisting);
        if (opened.is_err()) { write_status(m, Status::FileNotFound); return; }
        std::vector<u8> bytes;
        std::vector<u8> chunk(64 * 1024);
        for (;;) {
            auto read = vfs->read(opened.value(), chunk);
            if (read.is_err() || read.value() == 0) break;
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read.value()));
        }
        (void)vfs->close(opened.value());
        // A file that holds no font is accepted and adds no family (probe
        // "addfile notfont st=0 count=0").
        add_faces(*c, dwrite::fonts_from_data(std::make_shared<const std::vector<u8>>(std::move(bytes))));
        write_status(m, Status::Ok);
    };
    map["GdipPrivateAddMemoryFont"] = [&mgr, add_faces](Marshaler& m) {
        GpFontCollection* c = mgr.objects().get<GpFontCollection>(handle_arg(m, 0));
        const GuestAddr data = m.arg_ptr(1);
        const i32 length = static_cast<i32>(m.arg32(2));
        if (c == nullptr || c->installed || data == 0 || length <= 0) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        // GDI+ copies the memory: the caller may free it at once (probe
        // "after zeroing source").
        auto bytes = std::make_shared<std::vector<u8>>(static_cast<usize>(length));
        if (m.memory().read_bytes(data, bytes->data(), bytes->size()).is_err()) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        add_faces(*c, dwrite::fonts_from_data(std::shared_ptr<const std::vector<u8>>(bytes)));
        write_status(m, Status::Ok);
    };

    // ---- Fonts ----
    map["GdipCreateFont"] = [&mgr](Marshaler& m) {
        const u64 fh = handle_arg(m, 0);
        const float em = real_arg(m, 1);
        const i32 style = static_cast<i32>(m.arg32(2));
        const i32 unit = static_cast<i32>(m.arg32(3));
        const GuestAddr out = m.arg_ptr(4);
        auto family = mgr.objects().share<GpFontFamily>(fh);
        if (!family || out == 0 || !(em > 0) || !std::isfinite(em) || !valid_font_unit(unit)) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        if (!family_face(*family, style)) { write_status(m, Status::FontStyleNotFound); return; }
        GpFont font;
        font.family = fh;
        font.family_object = std::move(family);
        font.em_size = em;
        font.style = style;
        font.unit = unit;
        write_handle(m, out, mgr.objects().add(std::move(font)));
        write_status(m, Status::Ok);
    };
    map["GdipCloneFont"] = [&mgr](Marshaler& m) {
        GpFont* f = mgr.objects().get<GpFont>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        GpFont copy = *f;
        write_handle(m, out, mgr.objects().add(std::move(copy)));
        write_status(m, Status::Ok);
    };
    map["GdipDeleteFont"] = [&mgr](Marshaler& m) {
        write_status(m, mgr.objects().remove<GpFont>(handle_arg(m, 0)) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipGetFamily"] = [&mgr](Marshaler& m) {
        GpFont* f = mgr.objects().get<GpFont>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        // Re-register the family if a private collection that owned it went
        // away: the font keeps its family alive, as GDI+ refcounts it.
        if (mgr.objects().get<GpFontFamily>(f->family) == nullptr) {
            f->family = mgr.objects().add(f->family_object);
        }
        write_handle(m, out, f->family);
        write_status(m, Status::Ok);
    };
    map["GdipGetFontStyle"] = [&mgr](Marshaler& m) {
        GpFont* f = mgr.objects().get<GpFont>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        (void)m.write_u32(out, static_cast<u32>(f->style));
        write_status(m, Status::Ok);
    };
    map["GdipGetFontSize"] = [&mgr](Marshaler& m) {
        GpFont* f = mgr.objects().get<GpFont>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        write_f32(m, out, f->em_size);
        write_status(m, Status::Ok);
    };
    map["GdipGetFontUnit"] = [&mgr](Marshaler& m) {
        GpFont* f = mgr.objects().get<GpFont>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        (void)m.write_u32(out, static_cast<u32>(f->unit));
        write_status(m, Status::Ok);
    };
    map["GdipGetFontHeight"] = [&mgr](Marshaler& m) {
        GpFont* f = mgr.objects().get<GpFont>(handle_arg(m, 0));
        const u64 gh = handle_arg(m, 1);
        const GuestAddr out = m.arg_ptr(2);
        if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        const float ls = font_line_spacing(*f);
        float height = ls;
        if (gh == 0) {
            // Without a graphics only the World/Pixel answer exists; Windows
            // faults for the other units (probe "nullheight unit=3..6").
            if (f->unit != UnitWorld && f->unit != UnitPixel) {
                write_status(m, Status::InvalidParameter);
                return;
            }
        } else {
            GpGraphics* g = mgr.objects().get<GpGraphics>(gh);
            if (g == nullptr) { write_status(m, Status::InvalidParameter); return; }
            // Line spacing in the graphics' page unit; a World font is in
            // world units already (probe "font pt10 pageunit=...").
            if (f->unit != UnitWorld) {
                const float device = ls * unit_to_device(f->unit, g->dpi_y);
                const float page = unit_to_device(g->state.page_unit, g->dpi_y) * g->state.page_scale;
                height = page != 0 ? device / page : device;
            }
        }
        write_f32(m, out, height);
        write_status(m, Status::Ok);
    };
    map["GdipGetFontHeightGivenDPI"] = [&mgr](Marshaler& m) {
        GpFont* f = mgr.objects().get<GpFont>(handle_arg(m, 0));
        const float dpi = real_arg(m, 1);
        const GuestAddr out = m.arg_ptr(2);
        if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        const float ls = font_line_spacing(*f);
        // World/Pixel ignore the dpi (probe "h96" == "h72" for units 0/2).
        const float height = (f->unit == UnitWorld || f->unit == UnitPixel) ? ls
                                                                              : ls * unit_to_device(f->unit, dpi);
        write_f32(m, out, height);
        write_status(m, Status::Ok);
    };
    auto get_logfont = [&mgr](Marshaler& m, bool wide) {
        GpFont* f = mgr.objects().get<GpFont>(handle_arg(m, 0));
        GpGraphics* g = mgr.objects().get<GpGraphics>(handle_arg(m, 1));
        const GuestAddr out = m.arg_ptr(2);
        if (f == nullptr || g == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        float px = 0;
        if (f->unit == UnitWorld) {
            const Affine w = world_to_device(*g);
            px = f->em_size * std::sqrt(w.m21 * w.m21 + w.m22 * w.m22);
        } else {
            // Page unit and scale do not enter (probe "pageunit=... h=-13").
            px = f->em_size * unit_to_device(f->unit, g->dpi_y);
        }
        u8 raw[92]{};
        const i32 height = -round_half_away(px);
        const i32 weight = (f->style & FontStyleBold) ? 700 : 400;
        std::memcpy(raw + 0, &height, 4);
        std::memcpy(raw + 16, &weight, 4);
        raw[20] = (f->style & FontStyleItalic) ? 1 : 0;
        raw[21] = (f->style & FontStyleUnderline) ? 1 : 0;
        raw[22] = (f->style & FontStyleStrikeout) ? 1 : 0;
        const std::u16string name = f->family_object->name.substr(0, 31);
        usize size = 92;
        if (wide) {
            for (usize i = 0; i < name.size(); ++i) {
                const u16 c = static_cast<u16>(name[i]);
                std::memcpy(raw + 28 + i * 2, &c, 2);
            }
        } else {
            size = 60;
            u8 abuf[32]{};
            const auto r = codepage::wide_to_multi_byte(codepage::kCpAcp, 0,
                                                        reinterpret_cast<const u16*>(name.data()),
                                                        name.size(), abuf, 31, nullptr, false);
            std::memcpy(raw + 28, abuf, std::min<usize>(r.count, 31));
        }
        if (m.memory().write_bytes(out, raw, size).is_err()) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        write_status(m, Status::Ok);
    };
    map["GdipGetLogFontW"] = [get_logfont](Marshaler& m) { get_logfont(m, true); };
    map["GdipGetLogFontA"] = [get_logfont](Marshaler& m) { get_logfont(m, false); };
    auto from_logfont = [&mgr](Marshaler& m, bool wide) {
        const u64 hdc = handle_arg(m, 0);
        const GuestAddr lf = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(2);
        if (hdc == 0 || lf == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
        GdiManager::FontInfo info;
        if (!read_logfont(m, lf, wide, info)) { write_status(m, Status::InvalidParameter); return; }
        u64 font = 0;
        const Status s = font_from_logfont(mgr, static_cast<Handle>(hdc), info, font);
        write_handle(m, out, font);
        write_status(m, s);
    };
    map["GdipCreateFontFromLogfontW"] = [from_logfont](Marshaler& m) { from_logfont(m, true); };
    map["GdipCreateFontFromLogfontA"] = [from_logfont](Marshaler& m) { from_logfont(m, false); };
    map["GdipCreateFontFromDC"] = [&mgr](Marshaler& m) {
        const u64 hdc = handle_arg(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (hdc == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
        GdiManager* gdi = mgr.gdi();
        GdiManager::FontInfo info;
        const GdiManager::FontInfo* selected =
            gdi != nullptr ? gdi->get_dc_font_info(static_cast<Handle>(hdc)) : nullptr;
        if (selected != nullptr) {
            info = *selected;
        } else {
            info.face_name = u"System";  // a DC's default font: the raster System font
        }
        u64 font = 0;
        const Status s = font_from_logfont(mgr, static_cast<Handle>(hdc), info, font);
        write_handle(m, out, font);
        write_status(m, s);
    };

    // ---- String formats ----
    map["GdipCreateStringFormat"] = [&mgr](Marshaler& m) {
        const i32 flags = static_cast<i32>(m.arg32(0));
        const u16 language = static_cast<u16>(m.arg32(1));
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        GpStringFormat f;
        f.flags = flags;
        f.language = language;
        write_handle(m, out, mgr.objects().add(std::move(f)));
        write_status(m, Status::Ok);
    };
    map["GdipStringFormatGetGenericDefault"] = [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        write_handle(m, out, generic_format(mgr, false));
        write_status(m, Status::Ok);
    };
    map["GdipStringFormatGetGenericTypographic"] = [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        write_handle(m, out, generic_format(mgr, true));
        write_status(m, Status::Ok);
    };
    map["GdipCloneStringFormat"] = [&mgr](Marshaler& m) {
        GpStringFormat* f = mgr.objects().get<GpStringFormat>(handle_arg(m, 0));
        const GuestAddr out = m.arg_ptr(1);
        if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        GpStringFormat copy = *f;
        copy.generic = false;  // a clone is the caller's own object
        write_handle(m, out, mgr.objects().add(std::move(copy)));
        write_status(m, Status::Ok);
    };
    map["GdipDeleteStringFormat"] = [&mgr](Marshaler& m) {
        const u64 h = handle_arg(m, 0);
        GpStringFormat* f = mgr.objects().get<GpStringFormat>(h);
        if (f == nullptr) { write_status(m, Status::InvalidParameter); return; }
        if (!f->generic) (void)mgr.objects().remove<GpStringFormat>(h);  // probe "after-delete same"
        write_status(m, Status::Ok);
    };
    auto format_get = [&mgr](HandlerMap& mp, const char* api, i32 (*pick)(const GpStringFormat&)) {
        mp[api] = [&mgr, pick](Marshaler& m) {
            GpStringFormat* f = mgr.objects().get<GpStringFormat>(handle_arg(m, 0));
            const GuestAddr out = m.arg_ptr(1);
            if (f == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
            (void)m.write_u32(out, static_cast<u32>(pick(*f)));
            write_status(m, Status::Ok);
        };
    };
    format_get(map, "GdipGetStringFormatFlags", [](const GpStringFormat& f) { return f.flags; });
    format_get(map, "GdipGetStringFormatAlign", [](const GpStringFormat& f) { return f.align; });
    format_get(map, "GdipGetStringFormatLineAlign", [](const GpStringFormat& f) { return f.line_align; });
    format_get(map, "GdipGetStringFormatTrimming", [](const GpStringFormat& f) { return f.trimming; });
    format_get(map, "GdipGetStringFormatHotkeyPrefix", [](const GpStringFormat& f) { return f.hotkey; });
    format_get(map, "GdipGetStringFormatTabStopCount",
               [](const GpStringFormat& f) { return static_cast<i32>(f.tabs.size()); });
    format_get(map, "GdipGetStringFormatMeasurableCharacterRangeCount",
               [](const GpStringFormat& f) { return static_cast<i32>(f.ranges.size()); });
    auto format_set = [&mgr](HandlerMap& mp, const char* api, i32 lo, i32 hi, i32 GpStringFormat::*field) {
        mp[api] = [&mgr, lo, hi, field](Marshaler& m) {
            GpStringFormat* f = mgr.objects().get<GpStringFormat>(handle_arg(m, 0));
            const i32 v = static_cast<i32>(m.arg32(1));
            // Out-of-range enum values are refused and leave the format as
            // it was (probe "set align 3", "set trim 6", "set hotkey 3").
            if (f == nullptr || v < lo || v > hi) { write_status(m, Status::InvalidParameter); return; }
            f->*field = v;
            write_status(m, Status::Ok);
        };
    };
    format_set(map, "GdipSetStringFormatAlign", 0, 2, &GpStringFormat::align);
    format_set(map, "GdipSetStringFormatLineAlign", 0, 2, &GpStringFormat::line_align);
    format_set(map, "GdipSetStringFormatTrimming", 0, 5, &GpStringFormat::trimming);
    format_set(map, "GdipSetStringFormatHotkeyPrefix", 0, 2, &GpStringFormat::hotkey);
    map["GdipSetStringFormatFlags"] = [&mgr](Marshaler& m) {
        GpStringFormat* f = mgr.objects().get<GpStringFormat>(handle_arg(m, 0));
        if (f == nullptr) { write_status(m, Status::InvalidParameter); return; }
        f->flags = static_cast<i32>(m.arg32(1));  // stored verbatim (probe "0x1234")
        write_status(m, Status::Ok);
    };
    map["GdipSetStringFormatDigitSubstitution"] = [&mgr](Marshaler& m) {
        GpStringFormat* f = mgr.objects().get<GpStringFormat>(handle_arg(m, 0));
        if (f == nullptr) { write_status(m, Status::InvalidParameter); return; }
        // Any StringDigitSubstitute value is stored (probe "set digit -1/4").
        f->digit_language = static_cast<u16>(m.arg32(1));
        f->digit_substitute = static_cast<i32>(m.arg32(2));
        write_status(m, Status::Ok);
    };
    map["GdipGetStringFormatDigitSubstitution"] = [&mgr](Marshaler& m) {
        GpStringFormat* f = mgr.objects().get<GpStringFormat>(handle_arg(m, 0));
        const GuestAddr lang = m.arg_ptr(1);
        const GuestAddr sub = m.arg_ptr(2);
        if (f == nullptr) { write_status(m, Status::InvalidParameter); return; }
        // Both outputs are optional (probe "digit null lang/sub", "both null").
        if (lang != 0) (void)m.write_u16(lang, f->digit_language);
        if (sub != 0) (void)m.write_u32(sub, static_cast<u32>(f->digit_substitute));
        write_status(m, Status::Ok);
    };
    map["GdipSetStringFormatTabStops"] = [&mgr](Marshaler& m) {
        GpStringFormat* f = mgr.objects().get<GpStringFormat>(handle_arg(m, 0));
        const float first = real_arg(m, 1);
        const i32 count = static_cast<i32>(m.arg32(2));
        const GuestAddr tabs = m.arg_ptr(3);
        if (f == nullptr) { write_status(m, Status::InvalidParameter); return; }
        // count <= 0 is Ok and changes nothing (probe "tabs set count0",
        // "count-1"); a NULL array with tabs is InvalidParameter; negative
        // offsets are GDI+'s own NotImplemented (probe "negative ...").
        if (count <= 0) { write_status(m, Status::Ok); return; }
        if (tabs == 0) { write_status(m, Status::InvalidParameter); return; }
        std::vector<float> values(static_cast<usize>(count));
        for (i32 i = 0; i < count; ++i) {
            if (!read_f32(m, tabs + static_cast<u64>(i) * 4, values[static_cast<usize>(i)])) {
                write_status(m, Status::InvalidParameter);
                return;
            }
        }
        if (first < 0 || std::any_of(values.begin(), values.end(), [](float v) { return v < 0; })) {
            write_status(m, Status::NotImplemented);
            return;
        }
        f->first_tab = first;
        f->tabs = std::move(values);
        write_status(m, Status::Ok);
    };
    map["GdipGetStringFormatTabStops"] = [&mgr](Marshaler& m) {
        GpStringFormat* f = mgr.objects().get<GpStringFormat>(handle_arg(m, 0));
        const i32 count = static_cast<i32>(m.arg32(1));
        const GuestAddr first = m.arg_ptr(2);
        const GuestAddr tabs = m.arg_ptr(3);
        // A negative count faults on Windows (probe "err 1"): refused here.
        if (f == nullptr || first == 0 || tabs == 0 || count < 0) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        write_f32(m, first, f->first_tab);
        const usize n = std::min(f->tabs.size(), static_cast<usize>(count));
        for (usize i = 0; i < n; ++i) write_f32(m, tabs + i * 4, f->tabs[i]);
        write_status(m, Status::Ok);
    };
    map["GdipSetStringFormatMeasurableCharacterRanges"] = [&mgr](Marshaler& m) {
        GpStringFormat* f = mgr.objects().get<GpStringFormat>(handle_arg(m, 0));
        const i32 count = static_cast<i32>(m.arg32(1));
        const GuestAddr ranges = m.arg_ptr(2);
        if (f == nullptr) { write_status(m, Status::InvalidParameter); return; }
        // At most 32 ranges (ValueOverflow, format unchanged: probe "set33").
        if (count > 32) { write_status(m, Status::ValueOverflow); return; }
        if (count > 0 && ranges == 0) { write_status(m, Status::InvalidParameter); return; }
        std::vector<CharacterRange> values;
        for (i32 i = 0; i < count; ++i) {
            const auto a = m.read_u32(ranges + static_cast<u64>(i) * 8);
            const auto b = m.read_u32(ranges + static_cast<u64>(i) * 8 + 4);
            if (a.is_err() || b.is_err()) { write_status(m, Status::InvalidParameter); return; }
            values.push_back({static_cast<i32>(a.value()), static_cast<i32>(b.value())});
        }
        f->ranges = std::move(values);
        write_status(m, Status::Ok);
    };

    register_text_layout(mgr, map);
}

}  // namespace vitreo::winapi::gdip
