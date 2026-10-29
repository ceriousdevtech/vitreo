// SPDX-License-Identifier: LGPL-2.1-only

/// @file getoutlinetextmetrics.cpp
/// @brief GetOutlineTextMetricsA/W — TrueType outline metrics.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {

u32 align_up(u32 value, u32 alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

void write_u32(std::vector<u8>& buf, u32 offset, u32 value) {
    if (offset + 4 > buf.size()) buf.resize(offset + 4);
    std::memcpy(buf.data() + offset, &value, 4);
}

void write_i32(std::vector<u8>& buf, u32 offset, i32 value) {
    write_u32(buf, offset, static_cast<u32>(value));
}

void write_ptr_offset(std::vector<u8>& buf, u32 offset, u32 value, bool is_64bit) {
    write_u32(buf, offset, value);
    if (is_64bit) write_u32(buf, offset + 4, 0);
}

std::string ansi_from_utf16(const std::u16string& text) {
    std::string out;
    out.reserve(text.size());
    for (char16_t ch : text) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

u32 append_wide(std::vector<u8>& buf, const std::u16string& text) {
    const u32 offset = align_up(static_cast<u32>(buf.size()), 2);
    buf.resize(offset);
    for (char16_t ch : text) {
        buf.push_back(static_cast<u8>(ch & 0xFF));
        buf.push_back(static_cast<u8>((ch >> 8) & 0xFF));
    }
    buf.push_back(0);
    buf.push_back(0);
    return offset;
}

u32 append_ansi(std::vector<u8>& buf, const std::string& text) {
    const u32 offset = static_cast<u32>(buf.size());
    buf.insert(buf.end(), text.begin(), text.end());
    buf.push_back(0);
    return offset;
}

}  // namespace

u32 GdiManager::get_outline_text_metrics(
    Handle hdc, u32 cj_copy, GuestAddr potm, bool wide, bool is_64bit) {
    ApiLock api_lock(*this);
    // A NULL or unknown DC fails with the last error untouched
    // (s3gdi_otm_probe "otm(NULL dc)=0 err=57005").
    auto face = text_face(hdc);
    if (!face) return 0;

    // OUTLINETEXTMETRIC: otmSize, TEXTMETRIC (W 60 / A 56 bytes), a filler
    // byte, PANOSE, the metrics block and four name offsets (PSTR, 4 or 8
    // bytes); x64 W is 232 bytes and x86 216, the names follow
    // (s3gdi_otm_probe "offs=232,...").
    const u32 tm_bytes = wide ? 60u : 56u;
    const u32 ptr_size = is_64bit ? 8u : 4u;
    const u32 panose_end = 4u + tm_bytes + 1u + 10u;
    const u32 metrics_off = align_up(panose_end, 4);
    const u32 names_off = align_up(metrics_off + 124u, ptr_size);
    const u32 struct_size = names_off + 4u * ptr_size;

    auto table = [&](u32 id) {
        auto bytes = dwrite::font_table(face->font, id);
        return bytes ? std::move(*bytes) : std::vector<u8>{};
    };
    auto be16 = [](const std::vector<u8>& t, usize o) -> u32 {
        return o + 2 <= t.size() ? (static_cast<u32>(t[o]) << 8) | t[o + 1] : 0u;
    };
    auto bes16 = [&](const std::vector<u8>& t, usize o) { return static_cast<i32>(static_cast<i16>(be16(t, o))); };
    auto tag = [](const char* s) {
        return static_cast<u32>(static_cast<u8>(s[0])) | (static_cast<u32>(static_cast<u8>(s[1])) << 8) |
               (static_cast<u32>(static_cast<u8>(s[2])) << 16) | (static_cast<u32>(static_cast<u8>(s[3])) << 24);
    };
    const auto head = table(tag("head"));
    const auto hhea = table(tag("hhea"));
    const auto os2 = table(tag("OS/2"));
    const auto post = table(tag("post"));
    const auto name = table(tag("name"));
    const double scale = face->scale;
    auto sc = [&](i32 v) { return static_cast<i32>(std::lround(v * scale)); };

    // The 'name' strings, Windows platform (US English first).
    auto name_string = [&](u32 id) -> std::u16string {
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
    };
    // family = name 1, face = name 4, style = name 2, and the "full" name
    // GDI hands back is the unique identifier, name 3 ("Monotype:Arial
    // Regular (Microsoft)").
    std::u16string family = name_string(1), face_name = name_string(4), style = name_string(2),
                   full = name_string(3);
    if (family.empty()) family = face->font.family_name;
    if (face_name.empty()) face_name = face->font.face_name.empty() ? family : face->font.face_name;
    if (style.empty()) style = u"Regular";
    if (full.empty()) full = face_name;

    std::vector<u8> buf(struct_size, 0);
    const u32 family_off = wide ? append_wide(buf, family) : append_ansi(buf, ansi_from_utf16(family));
    const u32 face_off = wide ? append_wide(buf, face_name) : append_ansi(buf, ansi_from_utf16(face_name));
    const u32 style_off = wide ? append_wide(buf, style) : append_ansi(buf, ansi_from_utf16(style));
    const u32 full_off = wide ? append_wide(buf, full) : append_ansi(buf, ansi_from_utf16(full));
    write_ptr_offset(buf, names_off + 0 * ptr_size, family_off, is_64bit);
    write_ptr_offset(buf, names_off + 1 * ptr_size, face_off, is_64bit);
    write_ptr_offset(buf, names_off + 2 * ptr_size, style_off, is_64bit);
    write_ptr_offset(buf, names_off + 3 * ptr_size, full_off, is_64bit);
    write_u32(buf, 0, static_cast<u32>(buf.size()));

    // TEXTMETRIC as GetTextMetrics writes it, then PANOSE after the filler.
    {
        u8 tm[60] = {};
        if (!text_metrics_bytes(hdc, wide, tm)) return 0;
        std::memcpy(buf.data() + 4, tm, wide ? 60 : 53);
    }
    if (os2.size() >= 42) std::memcpy(buf.data() + 4 + tm_bytes + 1, os2.data() + 32, 10);

    const i32 upem = head.size() >= 20 ? static_cast<i32>(be16(head, 18)) : 2048;
    write_u32(buf, metrics_off + 0, os2.size() >= 64 ? be16(os2, 62) : 0x40u);       // otmfsSelection
    write_u32(buf, metrics_off + 4, os2.size() >= 10 ? be16(os2, 8) : 0u);           // otmfsType
    write_i32(buf, metrics_off + 8, hhea.size() >= 22 ? bes16(hhea, 18) : 1);        // caret slope rise
    write_i32(buf, metrics_off + 12, hhea.size() >= 22 ? bes16(hhea, 20) : 0);       // caret slope run
    i32 italic_angle = 0;
    if (post.size() >= 8) {
        const i32 fixed = static_cast<i32>((be16(post, 4) << 16) | be16(post, 6));
        italic_angle = static_cast<i32>(std::lround(fixed / 65536.0 * 10.0));
    }
    write_i32(buf, metrics_off + 16, italic_angle);                                  // tenths of a degree
    write_u32(buf, metrics_off + 20, static_cast<u32>(upem));                        // otmEMSquare
    // Typographic ascender/descender/gap, scaled (Arial -13: 9/-3/2).
    write_i32(buf, metrics_off + 24, os2.size() >= 74 ? sc(bes16(os2, 68)) : face->ascent);
    write_i32(buf, metrics_off + 28, os2.size() >= 74 ? sc(bes16(os2, 70)) : -face->descent);
    write_u32(buf, metrics_off + 32, static_cast<u32>(std::max(0, os2.size() >= 74 ? sc(bes16(os2, 72)) : 0)));
    // otmsCapEmHeight / otmsXHeight are half and a quarter of the em on
    // Windows (7/3 at 13 ppem, 12/6 at 24, 8/4 at 16) - not the OS/2 values.
    write_u32(buf, metrics_off + 36, static_cast<u32>(std::lround(face->ppem / 2.0)));
    write_u32(buf, metrics_off + 40, static_cast<u32>(std::lround(face->ppem / 4.0)));
    if (head.size() >= 54) {
        write_i32(buf, metrics_off + 44, sc(bes16(head, 36)));  // xMin
        write_i32(buf, metrics_off + 48, sc(bes16(head, 42)));  // yMax
        write_i32(buf, metrics_off + 52, sc(bes16(head, 40)));  // xMax
        write_i32(buf, metrics_off + 56, sc(bes16(head, 38)));  // yMin
    }
    write_i32(buf, metrics_off + 60, hhea.size() >= 10 ? sc(bes16(hhea, 4)) : face->ascent);
    write_i32(buf, metrics_off + 64, hhea.size() >= 10 ? sc(bes16(hhea, 6)) : -face->descent);
    write_u32(buf, metrics_off + 68, static_cast<u32>(std::max(0, hhea.size() >= 10 ? sc(bes16(hhea, 8)) : 0)));
    write_u32(buf, metrics_off + 72, head.size() >= 48 ? be16(head, 46) : 9u);      // lowestRecPPEM
    if (os2.size() >= 30) {
        for (u32 k = 0; k < 8; ++k) write_i32(buf, metrics_off + 76 + k * 4, sc(bes16(os2, 10 + k * 2)));
        write_u32(buf, metrics_off + 108, static_cast<u32>(std::max(1, sc(bes16(os2, 26)))));
        write_i32(buf, metrics_off + 112, sc(bes16(os2, 28)));
    }
    if (post.size() >= 12) {
        write_i32(buf, metrics_off + 116, std::max(1, sc(bes16(post, 10))));
        write_i32(buf, metrics_off + 120, sc(bes16(post, 8)));
    }

    const u32 needed = static_cast<u32>(buf.size());
    if (potm == 0) return needed;
    if (!m_guest_memory) return 0;
    // A short buffer receives its first cjCopy bytes and the call returns
    // cjCopy ("small(100) r=100 ... size=342").
    const u32 copy = std::min(cj_copy, needed);
    if (m_guest_memory->write_bytes(potm, buf.data(), copy).is_err()) return 0;
    return copy;
}

}  // namespace vitreo::winapi
