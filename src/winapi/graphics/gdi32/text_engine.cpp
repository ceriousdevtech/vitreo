// SPDX-License-Identifier: LGPL-2.1-only

/// @file text_engine.cpp
/// @brief GDI text: font mapping, TEXTMETRIC, advances, TextOut and DrawText.
///
/// GDI lays text out with a TrueType face's *device* metrics, not its linear
/// ones: the ascent and descent come from the VDMX table and each glyph's
/// advance from the hdmx record for the pixel size, when the face carries
/// them. tools/oracle_probe/s3gdi_text_probe shows Windows 11 matching those
/// tables exactly (Arial -13: VDMX 13/-3 gives tmHeight 16; hdmx gives
/// 'a'..'e' = 7); a face without them falls back to the scaled OS/2 metrics.
/// macOS ships the same files for the core Windows families (Arial, Courier
/// New, Georgia, Times New Roman, Verdana, Microsoft Sans Serif), so those
/// measure identically here.
///
/// Font mapping follows GDI (s3gdi_otm_probe "face ..."): the documented
/// logical-font aliases (capsule/registry/font_substitutes.hpp: "MS Shell
/// Dlg" -> Microsoft Sans Serif, "MS Shell Dlg 2" -> Tahoma) keep the
/// requested name for GetTextFace; the raster
/// families Windows ships as .fon files (System, MS Sans Serif, Courier,
/// Terminal, ...) have no TrueType file anywhere, so they are drawn with the
/// nearest TrueType face and keep their name; a face nobody has maps to
/// "MS Sans Serif", as an empty or unknown lfFaceName does on Windows.
/// Fonts the process added (AddFontMemResourceEx / AddFontResourceEx) come
/// before installed ones.
///
/// Drawing (s3gdi_textdraw_probe): the text cell is (x, y) to (x + extent,
/// y + tmHeight) after TA_* alignment; OPAQUE fills it with the background
/// colour; glyphs and fills write 0 into a 32bpp surface's reserved byte;
/// ETO_OPAQUE fills the rectangle and ETO_CLIPPED clips cell and glyphs to it.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/graphics/gdi32/bitmap_codec.hpp"

#include "capsule/registry/font_substitutes.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vitreo::winapi {

namespace {

constexpr u32 tag(char a, char b, char c, char d) {
    // DWRITE_MAKE_OPENTYPE_TAG: the first character in the low byte.
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}

u32 be16(const std::vector<u8>& t, usize o) {
    return o + 2 <= t.size() ? (static_cast<u32>(t[o]) << 8) | t[o + 1] : 0u;
}
i32 bes16(const std::vector<u8>& t, usize o) { return static_cast<i16>(be16(t, o)); }
u32 be32(const std::vector<u8>& t, usize o) { return (be16(t, o) << 16) | be16(t, o + 2); }

std::u16string fold(std::u16string_view s) {
    std::u16string out(s);
    for (auto& c : out) {
        if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c + 32);
    }
    return out;
}

std::vector<u8> table_of(const dwrite::MatchedFont& font, u32 id) {
    auto bytes = dwrite::font_table(font, id);
    return bytes ? std::move(*bytes) : std::vector<u8>{};
}

/// The tables GDI derives TEXTMETRIC from.
struct FaceTables {
    u32 upem = 2048;
    i32 hhea_ascent = 0, hhea_descent = 0, hhea_gap = 0;
    u32 advance_max = 0;
    u32 win_ascent = 0, win_descent = 0;
    i32 x_avg = 0;
    u32 weight_class = 400;
    u32 fs_selection = 0;
    u32 first_char = 32, last_char = 0xFFFF;
    std::array<u8, 10> panose{};
    bool fixed_pitch = false;
    u32 num_glyphs = 0;
    i32 bbox_width = 0;  ///< head xMax - xMin
    std::vector<u8> vdmx, hdmx, gasp;
};

FaceTables read_tables(const dwrite::MatchedFont& font) {
    FaceTables t;
    const auto head = table_of(font, tag('h', 'e', 'a', 'd'));
    const auto hhea = table_of(font, tag('h', 'h', 'e', 'a'));
    const auto os2 = table_of(font, tag('O', 'S', '/', '2'));
    const auto post = table_of(font, tag('p', 'o', 's', 't'));
    const auto maxp = table_of(font, tag('m', 'a', 'x', 'p'));
    if (head.size() >= 44) t.bbox_width = bes16(head, 40) - bes16(head, 36);
    t.upem = head.size() >= 20 && be16(head, 18) != 0 ? be16(head, 18)
             : font.metrics.design_units_per_em != 0 ? font.metrics.design_units_per_em
                                                     : 2048u;
    if (hhea.size() >= 12) {
        t.hhea_ascent = bes16(hhea, 4);
        t.hhea_descent = bes16(hhea, 6);
        t.hhea_gap = bes16(hhea, 8);
        t.advance_max = be16(hhea, 10);
    } else {
        t.hhea_ascent = font.metrics.ascent;
        t.hhea_descent = -static_cast<i32>(font.metrics.descent);
        t.hhea_gap = font.metrics.line_gap;
    }
    if (os2.size() >= 78) {
        t.x_avg = bes16(os2, 2);
        t.weight_class = be16(os2, 4);
        std::memcpy(t.panose.data(), os2.data() + 32, 10);
        t.fs_selection = be16(os2, 62);
        t.first_char = be16(os2, 64);
        t.last_char = be16(os2, 66);
        t.win_ascent = be16(os2, 74);
        t.win_descent = be16(os2, 76);
    } else {
        t.win_ascent = font.metrics.ascent;
        t.win_descent = font.metrics.descent;
        t.weight_class = font.weight;
    }
    if (post.size() >= 16) t.fixed_pitch = be32(post, 12) != 0;
    t.num_glyphs = maxp.size() >= 6 ? be16(maxp, 4) : font.glyph_count;
    t.vdmx = table_of(font, tag('V', 'D', 'M', 'X'));
    t.hdmx = table_of(font, tag('h', 'd', 'm', 'x'));
    t.gasp = table_of(font, tag('g', 'a', 's', 'p'));
    return t;
}

/// The VDMX group for a 1:1 device (or the catch-all 0:0:0 ratio).
std::optional<usize> vdmx_group(const std::vector<u8>& v) {
    if (v.size() < 6) return std::nullopt;
    const u32 ratios = be16(v, 4);
    for (u32 i = 0; i < ratios; ++i) {
        const usize r = 6 + i * 4u;
        if (r + 4 > v.size()) return std::nullopt;
        const u32 x = v[r + 1], y0 = v[r + 2], y1 = v[r + 3];
        if ((x == 0 && y0 == 0 && y1 == 0) || (x == 1 && y0 <= 1 && 1 <= y1)) {
            const usize off = be16(v, 6 + ratios * 4u + i * 2u);
            if (off + 4 <= v.size()) return off;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool vdmx_lookup(const std::vector<u8>& v, u32 ppem, i32& ymax, i32& ymin) {
    const auto group = vdmx_group(v);
    if (!group) return false;
    const u32 recs = be16(v, *group);
    for (u32 i = 0; i < recs; ++i) {
        const usize e = *group + 4 + i * 6u;
        if (e + 6 > v.size()) return false;
        if (be16(v, e) == ppem) {
            ymax = bes16(v, e + 2);
            ymin = bes16(v, e + 4);
            return true;
        }
    }
    return false;
}

/// The ppem for a cell height (a positive lfHeight): the tallest VDMX
/// height that fits, at the smallest ppem giving it (s3gdi_text_probe:
/// Arial 16 -> ppem 13 although 14 is also 16 high; Courier New 20 -> 17 of
/// 17/18; Arial 11 -> 8). Disengaged past the table's range.
std::optional<u32> vdmx_ppem_for_cell(const std::vector<u8>& v, i32 cell) {
    const auto group = vdmx_group(v);
    if (!group) return std::nullopt;
    const u32 recs = be16(v, *group);
    std::optional<u32> best;
    i32 best_height = -1, max_height = 0;
    for (u32 i = 0; i < recs; ++i) {
        const usize e = *group + 4 + i * 6u;
        if (e + 6 > v.size()) break;
        const u32 ppem = be16(v, e);
        const i32 h = bes16(v, e + 2) - bes16(v, e + 4);
        max_height = std::max(max_height, h);
        if (h > cell) continue;
        if (h > best_height || (h == best_height && ppem < *best)) {
            best = ppem;
            best_height = h;
        }
    }
    if (max_height < cell) return std::nullopt;
    return best;
}

bool hdmx_record(const std::vector<u8>& h, u32 num_glyphs, u32 ppem, std::vector<u8>& widths,
                 u32& max_width) {
    if (h.size() < 8 || num_glyphs == 0) return false;
    const u32 records = be16(h, 2);
    const u32 size = be32(h, 4);
    for (u32 i = 0; i < records; ++i) {
        const usize r = 8 + static_cast<usize>(i) * size;
        if (r + 2 + num_glyphs > h.size()) return false;
        if (h[r] == ppem) {
            max_width = h[r + 1];
            widths.assign(h.begin() + static_cast<std::ptrdiff_t>(r + 2),
                          h.begin() + static_cast<std::ptrdiff_t>(r + 2 + num_glyphs));
            return true;
        }
    }
    return false;
}

/// gasp GASP_DOGRAY for @p ppem (no table: smoothing allowed).
bool gasp_allows_gray(const std::vector<u8>& g, u32 ppem) {
    if (g.size() < 4) return true;
    const u32 ranges = be16(g, 2);
    for (u32 i = 0; i < ranges; ++i) {
        const usize r = 4 + i * 4u;
        if (r + 4 > g.size()) break;
        if (ppem <= be16(g, r)) return (be16(g, r + 2) & 0x2u) != 0;
    }
    return true;
}

/// Windows' raster and vector .fon families: no TrueType file exists, so
/// they are drawn with the nearest TrueType face.
std::u16string raster_font_replacement(const std::u16string& folded) {
    if (folded == u"system" || folded == u"ms sans serif" || folded == u"small fonts") {
        return u"Microsoft Sans Serif";
    }
    if (folded == u"terminal" || folded == u"fixedsys" || folded == u"courier" ||
        folded == u"modern") {
        return u"Courier New";
    }
    if (folded == u"ms serif" || folded == u"roman") return u"Times New Roman";
    if (folded == u"script") return u"Comic Sans MS";
    return {};
}

std::u16string utf8_to_utf16(std::string_view s) {
    std::u16string out;
    for (usize i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp = 0xFFFD;
        usize n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<u8>(s[i + 1]) & 0x3Fu);
            n = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<u8>(s[i + 2]) & 0x3Fu);
            n = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<u8>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<u8>(s[i + 3]) & 0x3Fu);
            n = 4;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += n;
    }
    return out;
}

std::string utf16_to_utf8(std::u16string_view s) {
    std::string out;
    for (usize i = 0; i < s.size(); ++i) {
        u32 cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

/// Nearer of black and white (a pen or text colour on a 1bpp surface).
u32 nearest_mono_color(u32 colorref) {
    const i32 r = static_cast<i32>(colorref & 0xFF);
    const i32 g = static_cast<i32>((colorref >> 8) & 0xFF);
    const i32 b = static_cast<i32>((colorref >> 16) & 0xFF);
    const i32 to_black = r * r + g * g + b * b;
    const i32 to_white = (255 - r) * (255 - r) + (255 - g) * (255 - g) + (255 - b) * (255 - b);
    return to_white < to_black ? 0xFFFFFFu : 0u;
}

i32 scaled(i32 design, double scale) { return static_cast<i32>(std::lround(design * scale)); }

}  // namespace

// =============================================================================
// Font mapping and private fonts
// =============================================================================

std::u16string GdiManager::substitute_face(const std::u16string& face) {
    const std::u16string folded = fold(face);
    // Vitreo's documented logical-font aliases (font_substitutes.hpp), the
    // same set HKLM\...\FontSubstitutes publishes. Anything else is not an
    // alias and is matched (or falls back) as itself.
    for (const auto& entry : capsule::kFontSubstitutes) {
        if (fold(std::u16string(entry.alias)) == folded) return std::u16string(entry.target);
    }
    return face;
}

std::optional<GdiManager::FaceMatch> GdiManager::match_face(const std::u16string& requested,
                                                            u32 weight, bool italic) const {
    ApiLock api_lock(*this);
    constexpr u32 kStyleItalic = 2, kStretchNormal = 5;
    const u32 style = italic ? kStyleItalic : 0u;
    const u32 want = weight == 0 ? 400u : weight;
    auto find_family = [&](const std::u16string& family) -> std::optional<dwrite::MatchedFont> {
        const std::u16string key = fold(family);
        const dwrite::MatchedFont* best = nullptr;
        i32 best_score = 0;
        for (const auto& set : m_private_fonts) {
            for (const auto& f : set.faces) {
                if (fold(f.family_name) != key && fold(f.face_name) != key) continue;
                const i32 score = std::abs(static_cast<i32>(f.weight) - static_cast<i32>(want)) +
                                  (((f.style != 0) != italic) ? 1000 : 0);
                if (!best || score < best_score) {
                    best = &f;
                    best_score = score;
                }
            }
        }
        if (best) return *best;
        // Core Text answers an unknown name with Helvetica; GDI's mapper
        // treats an unknown lfFaceName as no name (s3gdi_text_probe:
        // "NoSuchFontXYZ" reports MS Sans Serif), so only an exact family
        // match counts here.
        auto sys = dwrite::match_system_font(family, want, kStretchNormal, style);
        if (!sys || (fold(sys->family_name) != key && fold(sys->postscript_name) != key))
            return std::nullopt;
        return sys;
    };
    if (!requested.empty()) {
        const std::u16string folded = fold(requested);
        const std::u16string raster = raster_font_replacement(folded);
        if (!raster.empty()) {
            if (auto f = find_family(raster)) return FaceMatch{std::move(*f), requested};
        } else {
            const std::u16string alias = substitute_face(requested);
            if (auto f = find_family(alias)) {
                // An alias keeps the requested name; a direct match reports
                // the face's own spelling ("arial" -> "Arial").
                std::u16string reported = fold(alias) != folded ? requested
                                          : f->family_name.empty() ? requested
                                                                   : f->family_name;
                return FaceMatch{std::move(*f), std::move(reported)};
            }
        }
    }
    if (auto f = find_family(u"Microsoft Sans Serif")) return FaceMatch{std::move(*f), u"MS Sans Serif"};
    if (auto f = dwrite::match_system_ui_font(want, kStretchNormal, style)) {
        std::u16string name = f->family_name;
        return FaceMatch{std::move(*f), std::move(name)};
    }
    return std::nullopt;
}

Handle GdiManager::add_font_mem_resource(std::span<const u8> data, u32* count) {
    ApiLock api_lock(*this);
    if (data.empty()) return 0;
    auto faces = dwrite::fonts_from_data(std::vector<u8>(data.begin(), data.end()));
    if (faces.empty()) return 0;
    const Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    if (h == 0) return 0;
    if (count) *count = static_cast<u32>(faces.size());
    m_private_fonts.push_back(PrivateFontSet{h, {}, std::move(faces)});
    m_text_faces.clear();
    return h;
}

bool GdiManager::remove_font_mem_resource(Handle h) {
    ApiLock api_lock(*this);
    const auto it = std::find_if(m_private_fonts.begin(), m_private_fonts.end(), [&](const PrivateFontSet& s) {
        return s.handle == h && s.source.empty();
    });
    if (it == m_private_fonts.end()) return false;
    m_private_fonts.erase(it);
    (void)m_handles.close(h);
    m_text_faces.clear();
    return true;
}

i32 GdiManager::add_font_resource(const std::u16string& path, u32* error) {
    ApiLock api_lock(*this);
    auto fail = [&](u32 code) {
        if (error) *error = code;
        return 0;
    };
    if (path.empty()) return fail(error_codes::ERROR_INVALID_PARAMETER);
    if (m_vfs == nullptr) return fail(error_codes::ERROR_FILE_NOT_FOUND);
    // A bare file name is looked up in the Fonts folder too.
    std::vector<std::u16string> candidates{path};
    if (path.find(u'\\') == std::u16string::npos && path.find(u'/') == std::u16string::npos) {
        candidates.push_back(u"C:\\Windows\\Fonts\\" + path);
    }
    for (const auto& candidate : candidates) {
        const std::string guest = utf16_to_utf8(candidate);
        auto opened = m_vfs->open(guest, capsule::AccessMode::Read, capsule::ShareMode::Read,
                                  capsule::CreationDisposition::OpenExisting);
        if (opened.is_err()) continue;
        std::vector<u8> bytes;
        std::vector<u8> chunk(64 * 1024);
        for (;;) {
            auto read = m_vfs->read(opened.value(), chunk);
            if (read.is_err() || read.value() == 0) break;
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read.value()));
        }
        (void)m_vfs->close(opened.value());
        auto faces = dwrite::fonts_from_data(bytes);
        if (faces.empty()) return fail(error_codes::ERROR_INVALID_DATA);
        const i32 n = static_cast<i32>(faces.size());
        const Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
        m_private_fonts.push_back(PrivateFontSet{h, fold(path), std::move(faces)});
        m_text_faces.clear();
        return n;
    }
    return fail(error_codes::ERROR_FILE_NOT_FOUND);
}

bool GdiManager::remove_font_resource(const std::u16string& path, u32* error) {
    ApiLock api_lock(*this);
    const std::u16string key = fold(path);
    const auto it = std::find_if(m_private_fonts.begin(), m_private_fonts.end(), [&](const PrivateFontSet& s) {
        return !s.source.empty() && s.source == key;
    });
    if (it == m_private_fonts.end()) {
        if (error) *error = error_codes::ERROR_FILE_NOT_FOUND;
        return false;
    }
    (void)m_handles.close(it->handle);
    m_private_fonts.erase(it);
    m_text_faces.clear();
    return true;
}

// =============================================================================
// Realising a logical font
// =============================================================================

std::shared_ptr<const GdiManager::TextFace> GdiManager::text_face(Handle hdc) const {
    ApiLock api_lock(*this);
    const auto dc = m_dc_states.find(hdc);
    if (dc == m_dc_states.end()) return nullptr;
    const Handle font_handle = dc->second.selected_font;
    if (const auto cached = m_text_faces.find(font_handle); cached != m_text_faces.end()) {
        return cached->second;
    }
    const FontInfo* info = lookup_font(font_handle);
    FontInfo fallback;  // SYSTEM_FONT's LOGFONT (s3gdi_info_probe "stockfont 13")
    fallback.face_name = u"System";
    fallback.height = 16;
    fallback.lf_weight = 700;
    if (info == nullptr) info = &fallback;

    const u32 lf_weight = static_cast<u32>(std::clamp(info->lf_weight, 0, 1000));
    auto matched = match_face(info->face_name, lf_weight, info->italic);
    if (!matched) return nullptr;
    auto face = realize_face(*info, std::move(*matched));
    m_text_faces[font_handle] = face;
    return face;
}

std::shared_ptr<GdiManager::TextFace> GdiManager::realize_face(const FontInfo& logical, FaceMatch matched) const {
    ApiLock api_lock(*this);
    const FontInfo* info = &logical;
    const u32 lf_weight = static_cast<u32>(std::clamp(info->lf_weight, 0, 1000));
    auto face = std::make_shared<TextFace>();
    face->font = std::move(matched.font);
    face->reported_face = std::move(matched.reported);
    const FaceTables t = read_tables(face->font);
    const u32 upem = t.upem;
    const u32 cell_units = t.win_ascent + t.win_descent != 0 ? t.win_ascent + t.win_descent : upem;

    // Pixels per em: a negative lfHeight is the em, a positive one the cell
    // (the largest VDMX size that fits, else scaled), zero is 12pt at 96 dpi
    // (s3gdi_text_probe "Arial 0" == "Arial -16").
    u32 ppem = 16;
    i32 lf_height = info->height;
    // The raster System font has one size, a 16-pixel cell, which is what
    // lfHeight 0 selects ("face System -> ... h=16").
    if (lf_height == 0 && fold(info->face_name) == u"system") lf_height = 16;
    if (lf_height < 0) {
        ppem = static_cast<u32>(-static_cast<i64>(lf_height));
    } else if (lf_height > 0) {
        if (auto v = vdmx_ppem_for_cell(t.vdmx, lf_height)) {
            ppem = *v;
        } else {
            ppem = static_cast<u32>(std::max<i64>(
                1, static_cast<i64>(std::floor(static_cast<double>(lf_height) * upem / cell_units + 0.5))));
        }
    }
    face->ppem = std::max<u32>(ppem, 1);
    face->scale = static_cast<double>(face->ppem) / upem;
    i32 ymax = 0, ymin = 0;
    if (vdmx_lookup(t.vdmx, face->ppem, ymax, ymin)) {
        face->ascent = ymax;
        face->descent = -ymin;
    } else {
        // No VDMX: the scaled Windows metrics, rounded (Arial Black -13:
        // 14/4 from 2254/634 units).
        face->ascent = scaled(static_cast<i32>(t.win_ascent), face->scale);
        face->descent = scaled(static_cast<i32>(t.win_descent), face->scale);
    }
    face->height = face->ascent + face->descent;
    face->internal_leading = face->height - static_cast<i32>(face->ppem);
    const i32 gap_units = t.hhea_ascent - t.hhea_descent + t.hhea_gap -
                          static_cast<i32>(t.win_ascent + t.win_descent);
    face->external_leading = std::max<i32>(0, scaled(gap_units, face->scale));
    face->ave_width = scaled(t.x_avg, face->scale);
    u32 hdmx_max = 0;
    (void)hdmx_record(t.hdmx, t.num_glyphs, face->ppem, face->device_widths, hdmx_max);
    // tmMaxCharWidth is the scaled font bounding box width, not the widest
    // advance (Arial -13: (4096 + 1361) units -> 35; Courier New -13 -> 10).
    face->max_width = t.bbox_width > 0 ? scaled(t.bbox_width, face->scale)
                                       : scaled(static_cast<i32>(t.advance_max), face->scale);
    face->weight = static_cast<i32>(t.weight_class);
    // A bold request on a face with no bold member is emboldened: one extra
    // pixel per advance, reported as FW_BOLD.
    if (lf_weight >= 600 && t.weight_class < 600) {
        face->simulated_bold = true;
        face->weight = 700;
        face->ave_width += 1;
        face->max_width += 1;
    }
    // s3gdi_text_probe / s3gdi_otm_probe "tmchars": the default character is
    // the first one less one, the break character the first one up to space.
    face->first_char = static_cast<u16>(t.first_char);
    face->last_char = static_cast<u16>(t.last_char);
    face->default_char = static_cast<u16>(t.first_char > 0 ? t.first_char - 1 : 31);
    face->break_char = static_cast<u16>(t.first_char > 0 && t.first_char < 32 ? t.first_char : 32);
    face->italic = (info->italic || (t.fs_selection & 1u)) ? 0xFF : 0;
    face->underlined = info->underline ? 0xFF : 0;
    face->struck_out = info->strike_out ? 0xFF : 0;
    // TMPF_FIXED_PITCH (set = variable) | TMPF_VECTOR | TMPF_TRUETYPE and the
    // PANOSE family: Arial 0x27, Courier New 0x36, Georgia 0x17.
    u8 family = 0x00;
    if (t.panose[0] == 2) {
        if (t.panose[3] == 9) family = 0x30;                                 // FF_MODERN
        else if (t.panose[1] >= 11 && t.panose[1] <= 13) family = 0x20;      // FF_SWISS
        else if (t.panose[1] >= 2 && t.panose[1] <= 10) family = 0x10;       // FF_ROMAN
    } else if (t.panose[0] == 3) {
        family = 0x40;                                                       // FF_SCRIPT
    } else if (t.panose[0] == 4 || t.panose[0] == 5) {
        family = 0x50;                                                       // FF_DECORATIVE
    }
    if (t.fixed_pitch && family == 0) family = 0x30;
    face->pitch_and_family = static_cast<u8>(family | 0x06 | (t.fixed_pitch ? 0 : 0x01));
    face->char_set = 0;  // ANSI_CHARSET, as Windows reports every Latin TrueType face
    const auto post = table_of(face->font, tag('p', 'o', 's', 't'));
    const auto os2 = table_of(face->font, tag('O', 'S', '/', '2'));
    face->underline_position = post.size() >= 12 ? scaled(bes16(post, 8), face->scale) : -1;
    face->underline_thickness = std::max(1, post.size() >= 12 ? scaled(bes16(post, 10), face->scale) : 1);
    face->strike_size = std::max(1, os2.size() >= 30 ? scaled(bes16(os2, 26), face->scale) : 1);
    face->strike_position = os2.size() >= 30 ? scaled(bes16(os2, 28), face->scale) : face->ascent / 3;
    // NONANTIALIASED_QUALITY draws aliased; ANTIALIASED_QUALITY follows the
    // face's gasp table (Arial has no grey at 9-16 ppem: "transparent
    // antialiased" wrote no partial pixels); every other quality is
    // ClearType on Windows and grey-smoothed here.
    if (info->quality == 3) face->antialias = false;
    else if (info->quality == 4) face->antialias = gasp_allows_gray(t.gasp, face->ppem);
    else face->antialias = true;
    return face;
}

std::u16string font_name_string(const dwrite::MatchedFont& font, u32 id) {
    // The 'name' table's Windows-platform string (US English first).
    const auto name = table_of(font, tag('n', 'a', 'm', 'e'));
    if (name.size() < 6) return {};
    const u32 count = be16(name, 2), storage = be16(name, 4);
    std::u16string best;
    i32 best_rank = -1;
    for (u32 i = 0; i < count; ++i) {
        const usize r = 6 + i * 12u;
        if (r + 12 > name.size()) break;
        const u32 platform = be16(name, r), encoding = be16(name, r + 2), language = be16(name, r + 4);
        if (be16(name, r + 6) != id) continue;
        const u32 len = be16(name, r + 8), off = storage + be16(name, r + 10);
        if (off + len > name.size()) continue;
        i32 rank = -1;
        std::u16string text;
        if (platform == 3 && (encoding == 1 || encoding == 0 || encoding == 10)) {
            rank = language == 0x409 ? 3 : 2;
            for (u32 k = 0; k + 1 < len; k += 2) {
                text.push_back(static_cast<char16_t>((name[off + k] << 8) | name[off + k + 1]));
            }
        } else if (platform == 1 && encoding == 0) {
            rank = 1;
            for (u32 k = 0; k < len; ++k) text.push_back(static_cast<char16_t>(name[off + k]));
        }
        if (rank > best_rank) {
            best_rank = rank;
            best = std::move(text);
        }
    }
    return best;
}

std::vector<u8> font_table_bytes(const dwrite::MatchedFont& font, u32 id) { return table_of(font, id); }

// =============================================================================
// Layout
// =============================================================================

bool GdiManager::layout_text(const TextFace& face, std::u16string_view text, i32 char_extra,
                             TextLayout& out) const {
    ApiLock api_lock(*this);
    out = {};
    out.unit_advances.assign(text.size(), 0);
    std::vector<u32> codepoints;
    std::vector<usize> units;
    for (usize i = 0; i < text.size();) {
        u32 cp = text[i];
        usize width = 1;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
            text[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            width = 2;
        }
        codepoints.push_back(cp);
        units.push_back(i);
        i += width;
    }
    if (codepoints.empty()) return true;
    auto glyphs = dwrite::glyph_indices(face.font, codepoints);
    if (!glyphs || glyphs->size() != codepoints.size()) return false;

    // Characters the face lacks come from a fallback face, as GDI's font
    // linking does.
    auto primary = std::make_shared<const dwrite::MatchedFont>(face.font);
    std::vector<std::shared_ptr<const dwrite::MatchedFont>> fonts(codepoints.size(), primary);
    for (usize c = 0; c < codepoints.size(); ++c) {
        if ((*glyphs)[c] != 0 || codepoints[c] < 0x20) continue;
        std::u16string one;
        if (codepoints[c] >= 0x10000) {
            const u32 v = codepoints[c] - 0x10000;
            one.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            one.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        } else {
            one.push_back(static_cast<char16_t>(codepoints[c]));
        }
        auto fb = dwrite::map_system_font(one, face.font.family_name, face.font.weight, 5, face.font.style,
                                          u"en-us");
        if (!fb || !fb->has_font) continue;
        auto mapped = dwrite::glyph_indices(fb->font, {codepoints[c]});
        if (!mapped || mapped->empty() || (*mapped)[0] == 0) continue;
        (*glyphs)[c] = (*mapped)[0];
        fonts[c] = std::make_shared<const dwrite::MatchedFont>(fb->font);
        out.used_fallback = true;
    }

    i32 pen = 0;
    for (usize c = 0; c < codepoints.size(); ++c) {
        const auto& font = fonts[c];
        const u16 g = (*glyphs)[c];
        i32 advance = 0;
        if (font == primary && g < face.device_widths.size()) {
            advance = face.device_widths[g];
        } else if (auto m = dwrite::glyph_metrics(*font, {g});
                   m && !m->empty() && font->metrics.design_units_per_em != 0) {
            advance = static_cast<i32>(std::lround(static_cast<double>((*m)[0].advance_width) * face.ppem /
                                                   font->metrics.design_units_per_em));
        }
        if (face.simulated_bold) advance += 1;
        advance += char_extra;
        if (out.runs.empty() || out.runs.back().font != font) out.runs.push_back(TextRun{font, {}, {}});
        out.runs.back().glyphs.push_back(g);
        out.runs.back().advances.push_back(advance);
        out.unit_advances[units[c]] = advance;
        pen += advance;
    }
    out.width = pen;
    return true;
}

bool GdiManager::draw_glyph_runs(RasterTarget& t, const TextFace& face, const TextLayout& layout,
                                 i32 origin_x, i32 baseline, const std::vector<i32>* dx_override,
                                 u32 color) {
    ApiLock api_lock(*this);
    const u32 text_rgb = t.mono ? nearest_mono_color(color) : (color & 0xFFFFFFu);
    usize glyph_index = 0;
    i32 pen = origin_x;
    for (const auto& run : layout.runs) {
        dwrite::GlyphRunRasterParams params;
        params.font = *run.font;
        params.font_em_size = static_cast<float>(face.ppem);
        params.baseline_origin_x = static_cast<float>(pen);
        params.baseline_origin_y = static_cast<float>(baseline);
        params.glyph_indices = run.glyphs;
        for (usize g = 0; g < run.glyphs.size(); ++g, ++glyph_index) {
            i32 advance = run.advances[g];
            if (dx_override && glyph_index < dx_override->size()) advance = (*dx_override)[glyph_index];
            params.glyph_advances.push_back(static_cast<float>(advance));
            params.glyph_offsets.push_back({});
            pen += advance;
        }
        // Aliased text is the smoothed coverage cut at one half: Core
        // Graphics' own aliased fill sets every pixel the outline touches,
        // which drew "Hi" at -13 with 78 pixels where Windows' hinted
        // rasterizer sets 33 (s3gdi_textdraw_probe "transparent mono").
        const bool smooth = face.antialias && !t.mono;
        auto raster = dwrite::rasterize_glyph_run(params, true);
        if (!raster) continue;
        if (!smooth) {
            for (auto& a : raster->alpha) a = a >= 128 ? 255 : 0;
        }
        const i32 w = raster->right - raster->left;
        // Emboldening draws the coverage again one pixel to the right.
        const i32 passes = face.simulated_bold ? 2 : 1;
        for (i32 pass = 0; pass < passes; ++pass) {
            for (i32 y = raster->top; y < raster->bottom; ++y) {
                for (i32 x = raster->left; x < raster->right; ++x) {
                    const u8 a = raster->alpha[static_cast<usize>(y - raster->top) * w + (x - raster->left)];
                    const i32 px = x + pass;
                    if (a == 0 || !raster_writable(t, px, y)) continue;
                    if (a == 255) {
                        raster_store(t, px, y, text_rgb);
                        continue;
                    }
                    const u8* p = t.surface->pixel_at(px, y);
                    const u32 inv = 255u - a;
                    u32 out = 0;
                    for (u32 c = 0; c < 3; ++c) {
                        const u32 s = (text_rgb >> (8 * c)) & 0xFFu;
                        out |= ((s * a + p[c] * inv + 127u) / 255u) << (8 * c);
                    }
                    raster_store(t, px, y, out);
                }
            }
        }
    }
    return true;
}

// =============================================================================
// TextOut / ExtTextOut
// =============================================================================

bool GdiManager::ext_text_out(Handle hdc, i32 x, i32 y, u32 options, const ClipRect* rect,
                              std::u16string_view text, const std::vector<i32>* dx, bool glyph_indices) {
    ApiLock api_lock(*this);
    constexpr u32 ETO_OPAQUE = 0x2, ETO_CLIPPED = 0x4;
    constexpr u32 TA_UPDATECP = 1, TA_RIGHT = 2, TA_CENTER = 6, TA_BOTTOM = 8, TA_BASELINE = 24;
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    DcState& dc = it->second;
    auto face = text_face(hdc);
    if (!face) return false;

    TextLayout layout;
    if (glyph_indices) {
        layout.unit_advances.assign(text.size(), 0);
        TextRun run{std::make_shared<const dwrite::MatchedFont>(face->font), {}, {}};
        std::vector<u16> glyphs(text.begin(), text.end());
        auto advances = glyph_advances(hdc, glyphs, nullptr);
        for (usize i = 0; i < glyphs.size(); ++i) {
            const i32 advance = (advances && i < advances->size() ? (*advances)[i] : 0) + dc.char_extra;
            run.glyphs.push_back(glyphs[i]);
            run.advances.push_back(advance);
            layout.unit_advances[i] = advance;
            layout.width += advance;
        }
        if (!run.glyphs.empty()) layout.runs.push_back(std::move(run));
    } else if (!layout_text(*face, text, dc.char_extra, layout)) {
        return false;
    }
    // lpDx holds one advance per character (per UTF-16 unit; a surrogate
    // pair's two entries both belong to its glyph).
    std::vector<i32> glyph_dx;
    i32 width = layout.width;
    if (dx != nullptr) {
        width = 0;
        for (usize i = 0; i < text.size() && i < dx->size(); ++i) width += (*dx)[i];
        for (usize u = 0; u < text.size(); ++u) {
            const i32 v = u < dx->size() ? (*dx)[u] : 0;
            const bool trail = !glyph_indices && u > 0 && text[u] >= 0xDC00 && text[u] < 0xE000 &&
                               text[u - 1] >= 0xD800 && text[u - 1] < 0xDC00;
            if (trail && !glyph_dx.empty()) glyph_dx.back() += v;
            else glyph_dx.push_back(v);
        }
    }

    // Reference point: the current position under TA_UPDATECP, then the
    // alignment moves it to the cell's left/top ("align ..." rows).
    i32 rx = x, ry = y;
    if (dc.text_align & TA_UPDATECP) {
        rx = dc.current_x;
        ry = dc.current_y;
    }
    i32 left = rx;
    if ((dc.text_align & TA_CENTER) == TA_CENTER) left = rx - width / 2;
    else if (dc.text_align & TA_RIGHT) left = rx - width;
    i32 top = ry;
    if ((dc.text_align & TA_BASELINE) == TA_BASELINE) top = ry - face->ascent;
    else if (dc.text_align & TA_BOTTOM) top = ry - face->height;
    if ((dc.text_align & TA_UPDATECP) && (dc.text_align & TA_CENTER) != TA_CENTER) {
        dc.current_x = (dc.text_align & TA_RIGHT) ? rx - width : rx + width;
    }

    RasterTarget t;
    if (!raster_target(hdc, t)) return true;  // a DC without a surface draws nothing
    const u32 bk = dc.bk_color & 0xFFFFFFu;
    const u32 bk_word = t.mono ? nearest_mono_color(bk) : bk;
    auto fill = [&](i32 l, i32 tp, i32 r, i32 b) {
        for (i32 py = std::max(tp, 0); py < std::min(b, t.surface->height); ++py) {
            for (i32 px = std::max(l, 0); px < std::min(r, t.surface->width); ++px) {
                if (raster_writable(t, px, py)) raster_store(t, px, py, bk_word);
            }
        }
    };
    ClipRect device_rect;
    if (rect) {
        device_rect.left = rect->left;
        device_rect.top = rect->top;
        device_rect.right = rect->right;
        device_rect.bottom = rect->bottom;
        lp_to_dp_rect(dc, device_rect.left, device_rect.top, device_rect.right, device_rect.bottom);
        if (device_rect.left > device_rect.right) std::swap(device_rect.left, device_rect.right);
        if (device_rect.top > device_rect.bottom) std::swap(device_rect.top, device_rect.bottom);
        device_rect.valid = true;
    }
    const ClipRect saved_clip = dc.clip_rect;
    if (rect && (options & ETO_OPAQUE)) {
        fill(device_rect.left, device_rect.top, device_rect.right, device_rect.bottom);
    }
    if (rect && (options & ETO_CLIPPED)) {
        dc.clip_rect = dc.clip_rect.valid ? ClipRect::combine(dc.clip_rect, device_rect, 1) : device_rect;
    }
    if (!text.empty()) {
        i32 dl = left, dt = top, dr = left + width, db = top + face->height;
        lp_to_dp_rect(dc, dl, dt, dr, db);
        if (dc.bk_mode == 2) fill(dl, dt, dr, db);  // OPAQUE: the text cell
        i32 bx = left, by = top + face->ascent;
        lp_to_dp_point(dc, bx, by);
        (void)draw_glyph_runs(t, *face, layout, bx, by, glyph_dx.empty() ? nullptr : &glyph_dx, dc.text_color);
        const u32 line_word = t.mono ? nearest_mono_color(dc.text_color) : (dc.text_color & 0xFFFFFFu);
        auto line = [&](i32 row, i32 thickness) {
            for (i32 k = 0; k < thickness; ++k) {
                for (i32 px = std::max(dl, 0); px < std::min(dr, t.surface->width); ++px) {
                    if (raster_writable(t, px, row + k)) raster_store(t, px, row + k, line_word);
                }
            }
        };
        // "underline transparent": one row at baseline + 1 for Arial -13
        // (post underlinePosition -1.38 px).
        if (face->underlined) line(by - std::min(-1, face->underline_position), face->underline_thickness);
        if (face->struck_out) line(by - face->strike_position, face->strike_size);
    }
    dc.clip_rect = saved_clip;
    finish_raster(t);
    return true;
}

bool GdiManager::text_out(Handle hdc, i32 x, i32 y, const std::string& utf8_text) {
    ApiLock api_lock(*this);
    const std::u16string text = utf8_to_utf16(utf8_text);
    return ext_text_out(hdc, x, y, 0, nullptr, text, nullptr, false);
}

// =============================================================================
// Metrics
// =============================================================================

bool GdiManager::write_text_metrics(Handle hdc, GuestAddr out, bool wide) {
    ApiLock api_lock(*this);
    if (!m_guest_memory || out == 0) return false;
    u8 buf[60] = {};
    if (!text_metrics_bytes(hdc, wide, buf)) return false;
    // TEXTMETRICW is written whole (its 3 padding bytes as zero); TEXTMETRICA
    // stops at tmCharSet, byte 53 (s3gdi_otm_probe "TEXTMETRICA ... bytes").
    return m_guest_memory->write_bytes(out, buf, wide ? 60 : 53).is_ok();
}

bool GdiManager::text_metrics_bytes(Handle hdc, bool wide, u8 (&buf)[60]) const {
    ApiLock api_lock(*this);
    auto face = text_face(hdc);
    if (!face) return false;
    std::memset(buf, 0, sizeof(buf));
    auto w32 = [&](usize off, i32 v) { std::memcpy(buf + off, &v, 4); };
    w32(0, face->height);
    w32(4, face->ascent);
    w32(8, face->descent);
    w32(12, face->internal_leading);
    w32(16, face->external_leading);
    w32(20, face->ave_width);
    w32(24, face->max_width);
    w32(28, face->weight);
    w32(32, 0);   // tmOverhang: 0 for TrueType
    w32(36, 96);  // tmDigitizedAspectX
    w32(40, 96);  // tmDigitizedAspectY
    usize o = 44;
    if (wide) {
        const u16 chars[4] = {face->first_char, face->last_char, face->default_char, face->break_char};
        std::memcpy(buf + o, chars, 8);
        o += 8;
    } else {
        // s3gdi_otm_probe "tmchars": the ANSI range starts one below the
        // default character (Arial W 32,65532,31,32 -> A 30,255,31,32).
        buf[o++] = static_cast<u8>(face->default_char > 0 ? std::min<u32>(face->default_char - 1u, 0xFFu) : 0u);
        buf[o++] = static_cast<u8>(std::min<u32>(face->last_char, 0xFF));
        buf[o++] = static_cast<u8>(std::min<u32>(face->default_char, 0xFF));
        buf[o++] = static_cast<u8>(std::min<u32>(face->break_char, 0xFF));
    }
    buf[o++] = face->italic;
    buf[o++] = face->underlined;
    buf[o++] = face->struck_out;
    buf[o++] = face->pitch_and_family;
    buf[o++] = face->char_set;
    return true;
}

bool GdiManager::get_text_metrics_w(Handle hdc, GuestAddr lptm) {
    ApiLock api_lock(*this);
    return write_text_metrics(hdc, lptm, true);
}

bool GdiManager::text_extent(Handle hdc, std::u16string_view text, i32& width, i32& height,
                             std::vector<i32>* cumulative, bool* used_fallback) {
    ApiLock api_lock(*this);
    auto face = text_face(hdc);
    const auto dc = m_dc_states.find(hdc);
    if (!face || dc == m_dc_states.end()) return false;
    TextLayout layout;
    if (!layout_text(*face, text, dc->second.char_extra, layout)) return false;
    width = layout.width;
    if (used_fallback) *used_fallback = layout.used_fallback;
    // An empty string measures 0 x 0 ("GetTextExtentPoint32W(empty)").
    height = text.empty() ? 0 : face->height;
    if (cumulative) {
        cumulative->clear();
        i32 sum = 0;
        for (const i32 a : layout.unit_advances) {
            sum += a;
            cumulative->push_back(sum);
        }
    }
    return true;
}

void GdiManager::get_text_extent(Handle hdc, const std::string& utf8_text, i32& out_width, i32& out_height) {
    ApiLock api_lock(*this);
    out_width = 0;
    out_height = 0;
    if (!text_extent(hdc, utf8_to_utf16(utf8_text), out_width, out_height)) {
        out_width = 0;
        out_height = 0;
    }
}

std::optional<std::vector<i32>> GdiManager::glyph_advances(Handle hdc, const std::vector<u16>& glyphs,
                                                           std::vector<std::array<i32, 3>>* abc) {
    ApiLock api_lock(*this);
    auto face = text_face(hdc);
    if (!face) return std::nullopt;
    std::vector<i32> out;
    if (glyphs.empty()) return out;
    auto metrics = dwrite::glyph_metrics(face->font, glyphs);
    if (!metrics || metrics->size() != glyphs.size()) return std::nullopt;
    const double scale = face->scale;
    for (usize i = 0; i < glyphs.size(); ++i) {
        const u16 g = glyphs[i];
        const auto& m = (*metrics)[i];
        i32 advance = g < face->device_widths.size() ? face->device_widths[g]
                                                     : static_cast<i32>(std::lround(m.advance_width * scale));
        if (face->simulated_bold) advance += 1;
        out.push_back(advance);
        if (abc) {
            // A and C are the scaled bearings and B the rest of the device
            // advance ("GetCharABCWidthsA(j)" = -1,3,1; 'W' = 0,13,0).
            // Windows measures the hinted outline, so at some sizes B is a
            // pixel narrower and the bearings a pixel wider (Arial -16 'A'
            // is 1,9,1 there and 0,11,0 here).
            const i32 a = static_cast<i32>(std::lround(m.left_side_bearing * scale));
            const i32 c = static_cast<i32>(std::lround(m.right_side_bearing * scale));
            const i32 b = std::max(1, advance - a - c);
            abc->push_back({a, b, advance - a - b});
        }
    }
    return out;
}

std::optional<std::vector<i32>> GdiManager::char_advances(Handle hdc, const std::vector<u32>& codepoints,
                                                          std::vector<std::array<i32, 3>>* abc) {
    ApiLock api_lock(*this);
    auto face = text_face(hdc);
    if (!face) return std::nullopt;
    if (codepoints.empty()) return std::vector<i32>{};
    auto glyphs = dwrite::glyph_indices(face->font, codepoints);
    if (!glyphs || glyphs->size() != codepoints.size()) return std::nullopt;
    return glyph_advances(hdc, *glyphs, abc);
}

i32 GdiManager::set_text_character_extra(Handle hdc, i32 extra) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return static_cast<i32>(0x80000000u);
    const i32 old = it->second.char_extra;
    it->second.char_extra = extra;
    return old;
}

i32 GdiManager::get_text_character_extra(Handle hdc) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    return it == m_dc_states.end() ? static_cast<i32>(0x80000000u) : it->second.char_extra;
}

u32 GdiManager::set_text_align_mode(Handle hdc, u32 align) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0xFFFFFFFFu;
    const u32 old = it->second.text_align;
    it->second.text_align = align;
    return old;
}

u32 GdiManager::get_text_align_mode(Handle hdc) const {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    return it == m_dc_states.end() ? 0xFFFFFFFFu : it->second.text_align;
}

// =============================================================================
// The font-data exports' view of the DC's font
// =============================================================================

std::optional<dwrite::MatchedFont> GdiManager::resolve_dc_font(Handle hdc) const {
    ApiLock api_lock(*this);
    auto face = text_face(hdc);
    if (!face) return std::nullopt;
    return face->font;
}

float GdiManager::dc_font_em_pixels(Handle hdc) const {
    ApiLock api_lock(*this);
    auto face = text_face(hdc);
    return face ? static_cast<float>(face->ppem) : 16.0f;
}

// =============================================================================
// DrawText (s3gdi_drawtext_probe)
// =============================================================================

i32 GdiManager::draw_text_ex(Handle hdc, std::u16string_view text, i32& left, i32& top, i32& right,
                             i32& bottom, u32 format, DrawTextParams* params) {
    ApiLock api_lock(*this);
    constexpr u32 DT_CENTER = 0x1, DT_RIGHT = 0x2, DT_VCENTER = 0x4, DT_BOTTOM = 0x8, DT_WORDBREAK = 0x10,
                  DT_SINGLELINE = 0x20, DT_EXPANDTABS = 0x40, DT_TABSTOP = 0x80, DT_NOCLIP = 0x100,
                  DT_EXTERNALLEADING = 0x200, DT_CALCRECT = 0x400, DT_NOPREFIX = 0x800,
                  DT_PATH_ELLIPSIS = 0x4000, DT_END_ELLIPSIS = 0x8000, DT_WORD_ELLIPSIS = 0x40000,
                  DT_HIDEPREFIX = 0x100000, DT_PREFIXONLY = 0x200000;
    auto dc_it = m_dc_states.find(hdc);
    if (dc_it == m_dc_states.end()) return 0;
    auto face = text_face(hdc);
    if (!face) return 0;
    const i32 char_extra = dc_it->second.char_extra;

    // DT_TABSTOP puts the tab size in bits 8-15, which then are not flags
    // ("calcrect tabstop 4": 4 << 8 is DT_CALCRECT's bit, yet it drew).
    u32 fmt = format;
    i32 tab_chars = 8;
    if (fmt & DT_TABSTOP) {
        tab_chars = static_cast<i32>((fmt >> 8) & 0xFFu);
        fmt &= ~0xFF00u;
    }
    if (params && params->tab_length > 0) tab_chars = params->tab_length;
    if (tab_chars <= 0) tab_chars = 8;
    const i32 tab_width = std::max(1, tab_chars * std::max(1, face->ave_width));
    const i32 left_margin = params ? params->left_margin : 0;
    const i32 right_margin = params ? params->right_margin : 0;
    const i32 line_height = face->height + ((fmt & DT_EXTERNALLEADING) ? face->external_leading : 0);
    const bool single = (fmt & DT_SINGLELINE) != 0;
    const i32 avail = (right - right_margin) - (left + left_margin);

    struct Line {
        std::u16string text;  ///< displayed characters (prefix marks and tabs removed)
        std::vector<i32> dx;  ///< advance of each displayed character, tabs folded in
        i32 lead = 0;         ///< tab space before the first character
        i32 width = 0;
        i32 underline = -1;   ///< displayed index of the prefixed character
    };
    // One logical line -> displayed characters under the prefix and tab rules.
    auto build = [&](std::u16string_view src) {
        Line line;
        std::u16string shown;
        i32 prefixed = -1;
        for (usize i = 0; i < src.size(); ++i) {
            const char16_t c = src[i];
            if (!(fmt & DT_NOPREFIX) && c == u'&') {
                if (i + 1 < src.size() && src[i + 1] == u'&') {
                    shown.push_back(u'&');
                    ++i;
                } else if (i + 1 < src.size()) {
                    prefixed = static_cast<i32>(shown.size());
                }
                continue;
            }
            shown.push_back(c);
        }
        std::u16string glyph_text;
        for (const char16_t c : shown) {
            if (c != u'\t') glyph_text.push_back(c);
        }
        TextLayout l;
        (void)layout_text(*face, glyph_text, char_extra, l);
        i32 pen = 0;
        usize g = 0;
        for (usize i = 0; i < shown.size(); ++i) {
            if (shown[i] == u'\t') {
                // Without DT_EXPANDTABS a tab takes no space ("calcrect tab").
                const i32 to = (fmt & DT_EXPANDTABS) ? (pen / tab_width + 1) * tab_width : pen;
                if (line.dx.empty()) line.lead += to - pen;
                else line.dx.back() += to - pen;
                pen = to;
                continue;
            }
            if (prefixed == static_cast<i32>(i)) line.underline = static_cast<i32>(line.text.size());
            const i32 a = g < l.unit_advances.size() ? l.unit_advances[g] : 0;
            ++g;
            line.text.push_back(shown[i]);
            line.dx.push_back(a);
            pen += a;
        }
        line.width = pen - line.lead;
        return line;
    };

    // Logical lines: CR, LF and CRLF each end one ("crlf", "cr only"); a
    // single line drops them ("singleline with newline").
    std::vector<std::u16string> paragraphs;
    {
        std::u16string cur;
        for (usize i = 0; i < text.size(); ++i) {
            const char16_t c = text[i];
            if (c == u'\r' || c == u'\n') {
                if (single) continue;
                paragraphs.push_back(cur);
                cur.clear();
                if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') ++i;
                continue;
            }
            cur.push_back(c);
        }
        paragraphs.push_back(cur);
    }
    std::vector<Line> lines;
    for (const auto& para : paragraphs) {
        if (!(fmt & DT_WORDBREAK) || single) {
            lines.push_back(build(para));
            continue;
        }
        // Break at the last space whose prefix fits; one space is consumed
        // per break and a word wider than the rectangle stays whole
        // ("calcrect wordbreak spaces" -> 3 lines, "longword" -> 105 wide).
        std::u16string_view rest = para;
        while (true) {
            Line whole = build(rest);
            if (whole.lead + whole.width <= avail || rest.empty()) {
                lines.push_back(std::move(whole));
                break;
            }
            usize cut = std::u16string_view::npos;
            for (usize i = 0; i < rest.size(); ++i) {
                if (rest[i] != u' ') continue;
                const Line prefix = build(rest.substr(0, i));
                if (prefix.lead + prefix.width <= avail) cut = i;
                else break;
            }
            if (cut == std::u16string_view::npos) {
                const usize space = rest.find(u' ');
                if (space == std::u16string_view::npos) {
                    lines.push_back(std::move(whole));
                    break;
                }
                cut = space;
            }
            // The breaking space counts toward the line's extent when it
            // still fits ("brown " is 38 wide in a 50 px rectangle).
            Line with_space = build(rest.substr(0, cut + 1));
            lines.push_back(with_space.lead + with_space.width <= avail ? std::move(with_space)
                                                                        : build(rest.substr(0, cut)));
            rest = rest.substr(cut + 1);
        }
    }

    // Ellipses replace the end (for a path, the middle) of a line that does
    // not fit ("end ellipsis": "He..." in 30 px).
    if (fmt & (DT_END_ELLIPSIS | DT_WORD_ELLIPSIS | DT_PATH_ELLIPSIS)) {
        const Line dots = build(u"...");
        for (auto& line : lines) {
            if (line.lead + line.width <= avail) continue;
            const std::u16string src = line.text;
            if ((fmt & DT_PATH_ELLIPSIS) && src.find(u'\\') != std::u16string::npos) {
                const usize last = src.rfind(u'\\');
                const std::u16string tail = src.substr(last);
                usize keep = last;
                while (keep > 0 && build(src.substr(0, keep) + u"..." + tail).width > avail) --keep;
                line = build(src.substr(0, keep) + u"..." + tail);
                continue;
            }
            usize keep = src.size();
            while (keep > 0) {
                const Line candidate = build(src.substr(0, keep));
                if (line.lead + candidate.width + dots.width <= avail) break;
                --keep;
            }
            const i32 lead = line.lead;
            line = build(src.substr(0, keep) + u"...");
            line.lead = lead;
        }
    }

    i32 max_width = 0;
    for (const auto& line : lines) max_width = std::max(max_width, line.lead + line.width);
    const i32 total = static_cast<i32>(lines.size()) * line_height;
    if (params) params->length_drawn = static_cast<u32>(text.size());
    if (fmt & DT_CALCRECT) {
        right = left + left_margin + max_width + right_margin;
        bottom = top + total;
        return total;
    }

    i32 y = top;
    if (single && (fmt & DT_VCENTER)) y = top + (bottom - top - line_height) / 2;
    else if (single && (fmt & DT_BOTTOM)) y = bottom - line_height;
    ClipRect clip;
    clip.left = left;
    clip.top = top;
    clip.right = right;
    clip.bottom = bottom;
    const bool clipped = (fmt & DT_NOCLIP) == 0;
    const i32 text_left = left + left_margin, text_right = right - right_margin;
    for (const auto& line : lines) {
        const i32 w = line.lead + line.width;
        i32 x = text_left;
        if (fmt & DT_CENTER) x = text_left + (text_right - text_left - w) / 2;
        else if (fmt & DT_RIGHT) x = text_right - w;
        x += line.lead;
        if (!(fmt & DT_PREFIXONLY) && !line.text.empty()) {
            (void)ext_text_out(hdc, x, y, clipped ? 0x4u : 0u, clipped ? &clip : nullptr, line.text, &line.dx,
                               false);
        }
        if (line.underline >= 0 && !(fmt & DT_HIDEPREFIX)) {
            // The prefix underline spans the character's advance one row
            // below the baseline ("prefixonly": 10..18 at y 19).
            i32 ux = x;
            for (i32 i = 0; i < line.underline; ++i) ux += line.dx[static_cast<usize>(i)];
            const i32 uw = line.dx[static_cast<usize>(line.underline)] - char_extra;
            RasterTarget t;
            if (raster_target(hdc, t)) {
                const i32 row = y + face->ascent - std::min(-1, face->underline_position);
                i32 l = ux, r = ux + uw, tt = row, bb = row + face->underline_thickness;
                lp_to_dp_rect(*t.dc, l, tt, r, bb);
                ClipRect dclip = clip;
                lp_to_dp_rect(*t.dc, dclip.left, dclip.top, dclip.right, dclip.bottom);
                const u32 word = t.mono ? nearest_mono_color(t.dc->text_color) : (t.dc->text_color & 0xFFFFFFu);
                for (i32 py = tt; py < bb; ++py) {
                    for (i32 px = l; px < r; ++px) {
                        if (clipped && (px < dclip.left || px >= dclip.right || py < dclip.top || py >= dclip.bottom)) {
                            continue;
                        }
                        if (raster_writable(t, px, py)) raster_store(t, px, py, word);
                    }
                }
                finish_raster(t);
            }
        }
        y += line_height;
    }
    if (single && (fmt & (DT_VCENTER | DT_BOTTOM))) return y - top;
    return total;
}

i32 GdiManager::draw_text(Handle hdc, const std::string& utf8_text, i32& left, i32& top, i32& right,
                          i32& bottom, u32 format) {
    ApiLock api_lock(*this);
    const std::u16string text = utf8_to_utf16(utf8_text);
    return draw_text_ex(hdc, text, left, top, right, bottom, format, nullptr);
}

}  // namespace vitreo::winapi
