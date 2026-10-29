// SPDX-License-Identifier: LGPL-2.1-only

/// @file font_enum.cpp
/// @brief EnumFontFamiliesEx: the callback sees what Windows 11 hands it.
///
/// tools/oracle_probe/s3gdi_enumfont_probe.c measured the entries: a named
/// family enumerates every style (sorted by full name: "Arial", "Arial Bold",
/// "Arial Bold Italic", "Arial Italic") and, for DEFAULT_CHARSET, each style
/// once per charset its OS/2 code-page bits claim, in the order Western,
/// Japanese, Hebrew, Arabic, Greek, Turkish, Baltic, Central European,
/// Cyrillic, Vietnamese. Each entry is an ENUMLOGFONTEX (the LOGFONT at 32
/// pixels per em - Arial: lfHeight 36, lfWidth 14 -, OUT_STROKE_PRECIS,
/// CLIP_STROKE_PRECIS, DRAFT_QUALITY, the full name, the style and the
/// script name) and a NEWTEXTMETRICEX (the TEXTMETRIC at that size, NTM_*
/// style flags, the em, cell and average width in design units, and the
/// OS/2 Unicode and code-page ranges), with TRUETYPE_FONTTYPE. An empty face
/// name enumerates one entry per family; an alias from FontSubstitutes
/// enumerates its target under the alias ("MS Shell Dlg" -> Microsoft Sans
/// Serif). The return value is the last callback's; a family nobody has
/// returns 1 with no calls; a NULL DC fails with ERROR_INVALID_HANDLE.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi {

namespace {

constexpr u32 tag(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}

struct CharsetInfo {
    u32 bit;          ///< OS/2 ulCodePageRange1 bit
    u8 charset;
    const char16_t* script;
};

/// Enumeration order as measured; Thai, Korean and Chinese positions and
/// names are Windows' but were not in the probe's fonts.
constexpr CharsetInfo kCharsets[] = {
    {0, 0, u"Western"},           {17, 128, u"Japanese"},        {19, 129, u"Hangul"},
    {18, 134, u"CHINESE_GB2312"}, {20, 136, u"CHINESE_BIG5"},    {5, 177, u"Hebrew"},
    {6, 178, u"Arabic"},          {3, 161, u"Greek"},            {4, 162, u"Turkish"},
    {7, 186, u"Baltic"},          {1, 238, u"Central European"}, {2, 204, u"Cyrillic"},
    {16, 222, u"Thai"},           {8, 163, u"Vietnamese"},       {21, 130, u"Hangul(Johab)"},
    {31, 2, u"Symbol"},
};

u32 be16(const std::vector<u8>& t, usize o) {
    return o + 2 <= t.size() ? (static_cast<u32>(t[o]) << 8) | t[o + 1] : 0u;
}
u32 be32(const std::vector<u8>& t, usize o) { return (be16(t, o) << 16) | be16(t, o + 2); }

std::u16string fold(std::u16string s) {
    for (auto& c : s) {
        if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c + 32);
    }
    return s;
}

void put_wide(std::vector<u8>& buf, usize off, const std::u16string& s, usize max_chars) {
    const usize n = std::min(s.size(), max_chars - 1);
    for (usize i = 0; i < n; ++i) {
        buf[off + i * 2] = static_cast<u8>(s[i] & 0xFF);
        buf[off + i * 2 + 1] = static_cast<u8>(s[i] >> 8);
    }
}

void put_ansi(std::vector<u8>& buf, usize off, const std::u16string& s, usize max_chars) {
    std::vector<u8> bytes(s.size() * 2 + 1);
    const auto r = codepage::wide_to_multi_byte(codepage::kCpAcp, 0, reinterpret_cast<const u16*>(s.data()),
                                                s.size(), bytes.data(), bytes.size(), nullptr, false);
    const usize n = std::min<usize>(r.count, max_chars - 1);
    std::memcpy(buf.data() + off, bytes.data(), n);
}

void put32(std::vector<u8>& buf, usize off, u32 v) { std::memcpy(buf.data() + off, &v, 4); }

}  // namespace

i32 GdiManager::enum_font_families_ex(Handle hdc, const std::u16string& face, u8 charset, GuestAddr proc,
                                      u64 lparam, bool wide, bool is_64bit) {
    ApiLock api_lock(*this);
    if (!m_dc_states.contains(hdc)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);  // "NULL dc ret=0 err=6"
        return 0;
    }
    if (proc == 0 || !m_guest_call || !m_guest_memory) return 1;

    const auto caller = m_guest_call;

    // The families to enumerate, with the name each is enumerated under.
    struct Family {
        std::u16string name;
        std::vector<dwrite::MatchedFont> styles;
    };
    std::vector<Family> families;
    auto styles_of = [&](const std::u16string& family) {
        std::vector<dwrite::MatchedFont> out;
        const std::u16string key = fold(family);
        for (const auto& set : m_private_fonts) {
            for (const auto& f : set.faces) {
                if (fold(f.family_name) == key) out.push_back(f);
            }
        }
        if (out.empty()) out = dwrite::enumerate_system_family_fonts(family);
        return out;
    };
    if (!face.empty()) {
        std::u16string target = substitute_face(face);
        // Raster families enumerate as the face GDI draws them with.
        if (auto m = match_face(face, 400, false); m && fold(target) == fold(face) &&
                                                  fold(m->font.family_name) != fold(face) &&
                                                  fold(m->reported) == fold(face)) {
            target = m->font.family_name;
        }
        Family f{face, styles_of(target)};
        if (f.styles.empty()) return 1;  // "missing ret=1 calls=0"
        if (fold(target) == fold(face) && !f.styles.empty() && !f.styles[0].family_name.empty()) {
            f.name = f.styles[0].family_name;
        }
        families.push_back(std::move(f));
    } else {
        std::vector<std::u16string> names;
        for (const auto& set : m_private_fonts) {
            for (const auto& f : set.faces) names.push_back(f.family_name);
        }
        for (auto& n : dwrite::enumerate_system_font_families()) {
            if (!n.empty() && n[0] != u'.') names.push_back(std::move(n));
        }
        std::vector<std::u16string> seen;
        for (const auto& n : names) {
            if (std::find(seen.begin(), seen.end(), fold(n)) != seen.end()) continue;
            seen.push_back(fold(n));
            auto styles = styles_of(n);
            if (styles.empty()) continue;
            // One entry per family: its regular (or first) style.
            auto regular = std::min_element(styles.begin(), styles.end(), [](const auto& a, const auto& b) {
                const i32 da = std::abs(static_cast<i32>(a.weight) - 400) + (a.style != 0 ? 1000 : 0);
                const i32 db = std::abs(static_cast<i32>(b.weight) - 400) + (b.style != 0 ? 1000 : 0);
                return da < db;
            });
            families.push_back({n, {*regular}});
        }
    }

    const usize elf_size = wide ? 348 : 188;
    const usize ntm_size = wide ? 100 : 96;
    const GuestAddr scratch = dib_scratch_alloc(static_cast<u32>(elf_size + ntm_size + 16));
    if (scratch == 0) return 0;
    const GuestAddr elf_addr = scratch;
    const GuestAddr ntm_addr = scratch + ((elf_size + 7) & ~usize{7});
    u64 last = 1;
    bool stop = false;
    for (auto& fam : families) {
        std::sort(fam.styles.begin(), fam.styles.end(), [](const auto& a, const auto& b) {
            return fold(font_name_string(a, 4)) < fold(font_name_string(b, 4));
        });
        for (const auto& style : fam.styles) {
            // The style's metrics at 32 pixels per em.
            FontInfo logical;
            logical.height = -32;
            logical.lf_weight = static_cast<i32>(style.weight);
            logical.italic = style.style != 0;
            const auto tf = realize_face(logical, FaceMatch{style, fam.name});
            u8 tm[60] = {};
            {
                // TEXTMETRIC bytes for this face (text_metrics_bytes needs a
                // DC; lay them out here the same way).
                auto w32 = [&](usize o, i32 v) { std::memcpy(tm + o, &v, 4); };
                w32(0, tf->height);
                w32(4, tf->ascent);
                w32(8, tf->descent);
                w32(12, tf->internal_leading);
                w32(16, tf->external_leading);
                w32(20, tf->ave_width);
                w32(24, tf->max_width);
                w32(28, tf->weight);
                w32(36, 96);
                w32(40, 96);
            }
            const auto os2 = font_table_bytes(style, tag('O', 'S', '/', '2'));
            const u32 csb1 = os2.size() >= 82 ? be32(os2, 78) : 1u;
            const u32 csb2 = os2.size() >= 86 ? be32(os2, 82) : 0u;
            const auto head = font_table_bytes(style, tag('h', 'e', 'a', 'd'));
            const u32 upem = head.size() >= 20 ? be16(head, 18) : 2048u;
            const u32 cell = os2.size() >= 78 ? be16(os2, 74) + be16(os2, 76) : upem;
            const u32 avg = os2.size() >= 4 ? be16(os2, 2) : 0u;
            const bool bold = style.weight >= 600;
            const bool italic = style.style != 0;
            const u32 ntm_flags = (italic ? 0x1u : 0u) | (bold ? 0x20u : 0u) | (!bold && !italic ? 0x40u : 0u);
            const bool fixed = (tf->pitch_and_family & 1u) == 0;
            const u8 lf_pitch = static_cast<u8>((tf->pitch_and_family & 0xF0u) | (fixed ? 1u : 2u));
            const std::u16string full = font_name_string(style, 4);
            const std::u16string style_name = font_name_string(style, 2);
            for (const auto& cs : kCharsets) {
                if (!(csb1 & (1u << cs.bit))) continue;
                if (charset != 1 && charset != cs.charset) continue;  // DEFAULT_CHARSET: all
                std::vector<u8> elf(elf_size, 0), ntm(ntm_size, 0);
                // LOGFONT
                put32(elf, 0, static_cast<u32>(tf->height));
                put32(elf, 4, static_cast<u32>(tf->ave_width));
                put32(elf, 16, static_cast<u32>(tf->weight));
                elf[20] = italic ? 0xFF : 0;
                elf[23] = cs.charset;
                elf[24] = 3;  // OUT_STROKE_PRECIS
                elf[25] = 2;  // CLIP_STROKE_PRECIS
                elf[26] = 1;  // DRAFT_QUALITY
                elf[27] = lf_pitch;
                const usize lf_size = wide ? 92 : 60;
                if (wide) {
                    put_wide(elf, 28, fam.name, 32);
                    put_wide(elf, lf_size, full.empty() ? fam.name : full, 64);
                    put_wide(elf, lf_size + 128, style_name, 32);
                    put_wide(elf, lf_size + 192, cs.script, 32);
                } else {
                    put_ansi(elf, 28, fam.name, 32);
                    put_ansi(elf, lf_size, full.empty() ? fam.name : full, 64);
                    put_ansi(elf, lf_size + 64, style_name, 32);
                    put_ansi(elf, lf_size + 96, cs.script, 32);
                }
                // NEWTEXTMETRICEX
                std::memcpy(ntm.data(), tm, 44);
                usize o = 44;
                if (wide) {
                    const u16 chars[4] = {tf->first_char, tf->last_char, tf->default_char, tf->break_char};
                    std::memcpy(ntm.data() + o, chars, 8);
                    o += 8;
                } else {
                    ntm[o++] = static_cast<u8>(tf->default_char > 0 ? std::min<u32>(tf->default_char - 1u, 255u) : 0u);
                    ntm[o++] = static_cast<u8>(std::min<u32>(tf->last_char, 255u));
                    ntm[o++] = static_cast<u8>(std::min<u32>(tf->default_char, 255u));
                    ntm[o++] = static_cast<u8>(std::min<u32>(tf->break_char, 255u));
                }
                ntm[o++] = italic ? 0xFF : 0;
                ntm[o++] = 0;
                ntm[o++] = 0;
                ntm[o++] = tf->pitch_and_family;
                ntm[o++] = cs.charset;
                const usize flags_at = wide ? 60 : 56;
                put32(ntm, flags_at, ntm_flags);
                put32(ntm, flags_at + 4, upem);
                put32(ntm, flags_at + 8, cell);
                put32(ntm, flags_at + 12, avg);
                for (u32 k = 0; k < 4; ++k) put32(ntm, flags_at + 16 + k * 4, os2.size() >= 58 ? be32(os2, 42 + k * 4) : 0u);
                put32(ntm, flags_at + 32, csb1);
                put32(ntm, flags_at + 36, csb2);
                if (m_guest_memory->write_bytes(elf_addr, elf.data(), elf.size()).is_err() ||
                    m_guest_memory->write_bytes(ntm_addr, ntm.data(), ntm.size()).is_err()) {
                    stop = true;
                    break;
                }
                const u64 args[4] = {elf_addr, ntm_addr, 4 /* TRUETYPE_FONTTYPE */, lparam};
                {
                    ApiUnlock unlocked(*this);
                    last = static_cast<u32>(caller(proc, args, 4, is_64bit));
                }
                if (static_cast<i32>(last) == 0) {
                    stop = true;
                    break;
                }
            }
            if (stop) break;
        }
        if (stop) break;
    }
    dib_scratch_free(scratch, static_cast<u32>(elf_size + ntm_size + 16));
    return static_cast<i32>(last);
}

}  // namespace vitreo::winapi
