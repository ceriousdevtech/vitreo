// SPDX-License-Identifier: LGPL-2.1-only

/// @file text_exports.cpp
/// @brief gdi32 text exports: argument marshalling and failure contracts.
///
/// Every return value and last-error rule here is measured on Windows 11
/// (x64 and x86) by tools/oracle_probe/s3gdi_text_probe.c,
/// s3gdi_textdraw_probe.c and s3gdi_otm_probe.c; the drawing and measuring
/// itself is the text engine in text_engine.cpp. Several of these exports
/// fail with the last error untouched (a NULL DC for TextOut, ExtTextOut,
/// GetTextMetrics, GetTextFace), and some successful ones clear it
/// (TextOut), so each rule is written out per export.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi::gdi_text_exports {

namespace {

/// @p count UTF-16 units from guest memory.
bool read_wide(Marshaler& m, GuestAddr p, usize count, std::u16string& out) {
    out.assign(count, u'\0');
    if (count == 0) return true;
    return m.memory().read_bytes(p, reinterpret_cast<u8*>(out.data()), count * 2).is_ok();
}

/// @p count bytes of ANSI text (CP_ACP) as UTF-16, one unit per byte for the
/// single-byte code pages.
bool read_ansi(Marshaler& m, GuestAddr p, usize count, std::u16string& out) {
    out.clear();
    if (count == 0) return true;
    std::vector<u8> bytes(count);
    if (m.memory().read_bytes(p, bytes.data(), count).is_err()) return false;
    std::vector<u16> units(count * 2 + 2);
    const auto r = codepage::multi_byte_to_wide(codepage::kCpAcp, 0, bytes.data(), bytes.size(), units.data(),
                                                units.size());
    if (!r.ok()) return false;
    out.assign(units.begin(), units.begin() + r.count);
    return true;
}

bool read_text(Marshaler& m, GuestAddr p, usize count, bool wide, std::u16string& out) {
    return wide ? read_wide(m, p, count, out) : read_ansi(m, p, count, out);
}

/// A NUL-terminated guest string (a font file path).
std::u16string read_cstring(Marshaler& m, GuestAddr p, bool wide) {
    std::u16string out;
    if (p == 0) return out;
    if (wide) {
        for (u32 i = 0; i < 32768; ++i) {
            const auto u = m.read_u16(p + i * 2);
            if (u.is_err() || u.value() == 0) break;
            out.push_back(static_cast<char16_t>(u.value()));
        }
        return out;
    }
    std::vector<u8> bytes;
    for (u32 i = 0; i < 32768; ++i) {
        const auto b = m.read_u8(p + i);
        if (b.is_err() || b.value() == 0) break;
        bytes.push_back(b.value());
    }
    std::vector<u16> units(bytes.size() + 1);
    const auto r = codepage::multi_byte_to_wide(codepage::kCpAcp, 0, bytes.data(), bytes.size(), units.data(),
                                                units.size());
    if (r.ok()) out.assign(units.begin(), units.begin() + r.count);
    return out;
}

/// Character codes first..last of a GetCharWidth/ABC request: UTF-16 for W,
/// ANSI bytes through CP_ACP for A ("GetCharWidthA(e9)": 0x80 measures as
/// U+20AC).
std::vector<u32> char_range(u32 first, u32 last, bool wide) {
    std::vector<u32> out;
    for (u32 c = first; c <= last; ++c) {
        if (wide) {
            out.push_back(c);
            continue;
        }
        const u8 byte = static_cast<u8>(c);
        u16 unit = byte;
        (void)codepage::multi_byte_to_wide(codepage::kCpAcp, 0, &byte, 1, &unit, 1);
        out.push_back(unit);
    }
    return out;
}

bool write_i32s(Marshaler& m, GuestAddr p, const std::vector<i32>& values) {
    if (values.empty()) return true;
    return m.memory().write_bytes(p, reinterpret_cast<const u8*>(values.data()), values.size() * 4).is_ok();
}

}  // namespace

// TextOutW/A(hdc, x, y, lpString, c)
void text_out(GdiManager& mgr, Marshaler& m, bool wide) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const i32 x = static_cast<i32>(m.arg32(1));
    const i32 y = static_cast<i32>(m.arg32(2));
    const GuestAddr str = m.arg_ptr(3);
    const i32 count = static_cast<i32>(m.arg32(4));
    // NULL DC and c < 0 fail with the last error untouched; c == 0 succeeds
    // without drawing (s3gdi_textdraw_probe "TextOutW(...)").
    if (!mgr.has_dc(hdc) || count < 0) {
        m.set_return(0u);
        return;
    }
    if (count == 0) {
        m.set_return(1u);
        return;
    }
    std::u16string text;
    if (str == 0 || !read_text(m, str, static_cast<usize>(count), wide, text)) {
        m.set_return(0u);
        return;
    }
    const bool ok = mgr.ext_text_out(hdc, x, y, 0, nullptr, text, nullptr, false);
    // A drawn string clears the last error (s3gdi_otm_probe "TextOutW ok=1 err=0").
    if (ok) set_last_error(0);
    m.set_return(ok ? 1u : 0u);
}

// ExtTextOutW/A(hdc, x, y, options, lprect, lpString, c, lpDx)
void ext_text_out(GdiManager& mgr, Marshaler& m, bool wide) {
    constexpr u32 ETO_GLYPH_INDEX = 0x10, ETO_PDY = 0x2000;
    const auto hdc = static_cast<Handle>(m.arg(0));
    const i32 x = static_cast<i32>(m.arg32(1));
    const i32 y = static_cast<i32>(m.arg32(2));
    const u32 options = m.arg32(3);
    const GuestAddr rect_ptr = m.arg_ptr(4);
    const GuestAddr str = m.arg_ptr(5);
    const u32 count = m.arg32(6);
    const GuestAddr dx_ptr = m.arg_ptr(7);
    if (!mgr.has_dc(hdc)) {  // "ExtTextOutW(NULL)=0 err=57005"
        m.set_return(0u);
        return;
    }
    const bool glyphs = (options & ETO_GLYPH_INDEX) != 0;
    std::u16string text;
    if (count > 0) {
        if (str == 0 || count > 0x7FFFFFFu) {
            m.set_return(0u);
            return;
        }
        const bool ok = (wide || glyphs) ? read_wide(m, str, count, text)
                                         : read_ansi(m, str, count, text);
        if (!ok) {
            m.set_return(0u);
            return;
        }
    }
    ClipRect rect;
    const ClipRect* rect_arg = nullptr;
    if (rect_ptr != 0) {
        i32 r[4] = {};
        if (m.memory().read_bytes(rect_ptr, reinterpret_cast<u8*>(r), 16).is_err()) {
            m.set_return(0u);
            return;
        }
        rect.left = r[0];
        rect.top = r[1];
        rect.right = r[2];
        rect.bottom = r[3];
        rect_arg = &rect;
    }
    std::vector<i32> dx;
    const std::vector<i32>* dx_arg = nullptr;
    if (dx_ptr != 0 && !text.empty()) {
        // ETO_PDY: lpDx holds (dx, dy) pairs; vertical offsets are not drawn.
        const usize n = (options & ETO_PDY) ? text.size() * 2 : text.size();
        std::vector<i32> raw(n);
        if (m.memory().read_bytes(dx_ptr, reinterpret_cast<u8*>(raw.data()), n * 4).is_err()) {
            m.set_return(0u);
            return;
        }
        if (options & ETO_PDY) {
            bool vertical = false;
            for (usize i = 0; i < text.size(); ++i) {
                dx.push_back(raw[i * 2]);
                vertical = vertical || raw[i * 2 + 1] != 0;
            }
            static bool logged = false;
            if (vertical && !logged) {
                logged = true;
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED gdi32!ExtTextOut: ETO_PDY vertical offsets\n");
            }
        } else {
            dx = std::move(raw);
        }
        dx_arg = &dx;
    }
    const bool ok = mgr.ext_text_out(hdc, x, y, options, rect_arg, text, dx_arg, glyphs);
    // Drawing characters clears the last error; glyph indices leave it
    // (s3gdi_otm_probe "ExtTextOutW ok=1 err=0", "(glyph index)=1 err=57005").
    if (ok && !glyphs && !text.empty()) set_last_error(0);
    m.set_return(ok ? 1u : 0u);
}

// GetTextExtentPoint32W/A and GetTextExtentPointW/A(hdc, lpString, c, lpSize)
void get_text_extent_point(GdiManager& mgr, Marshaler& m, bool wide) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr str = m.arg_ptr(1);
    const i32 count = static_cast<i32>(m.arg32(2));
    const GuestAddr size = m.arg_ptr(3);
    // NULL DC and c < 0: ERROR_INVALID_PARAMETER ("GetTextExtentPoint32W(NULL
    // dc)=0 err=87", "(c=-1)=0 err=87"). A NULL SIZE faults inside gdi32 on
    // Windows; it fails here the same way as a bad count.
    if (!mgr.has_dc(hdc) || count < 0 || size == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::u16string text;
    if (count > 0 && (str == 0 || !read_text(m, str, static_cast<usize>(count), wide, text))) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    i32 w = 0, h = 0;
    bool linked = false;
    if (!mgr.text_extent(hdc, text, w, h, nullptr, &linked)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const i32 out[2] = {w, h};
    (void)m.memory().write_bytes(size, reinterpret_cast<const u8*>(out), 8);
    // The x86 gdi32 measures through the language-pack path, which clears
    // the last error; x64 does too once a character needs a linked
    // (fallback) face ("GetTextExtentPoint32W ok=1 err=0" on x86 only,
    // "GetTextExtentPointW(cjk)=1 ... err=0" on both).
    if (count > 0 && (!m.cpu_is_64bit() || linked)) set_last_error(0);
    m.set_return(1u);
}

// GetTextExtentExPointW/A(hdc, lpszString, cchString, nMaxExtent, lpnFit, lpnDx, lpSize)
void get_text_extent_ex_point(GdiManager& mgr, Marshaler& m, bool wide) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr str = m.arg_ptr(1);
    const i32 count = static_cast<i32>(m.arg32(2));
    const i32 max_extent = static_cast<i32>(m.arg32(3));
    const GuestAddr fit_ptr = m.arg_ptr(4);
    const GuestAddr dx_ptr = m.arg_ptr(5);
    const GuestAddr size = m.arg_ptr(6);
    if (!mgr.has_dc(hdc)) {  // "GetTextExtentExPointW(NULL dc)=0 err=57005"
        m.set_return(0u);
        return;
    }
    if (count < 0) {  // "(c=-1)=0 err=0"
        set_last_error(0);
        m.set_return(0u);
        return;
    }
    if (size == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::u16string text;
    if (count > 0 && (str == 0 || !read_text(m, str, static_cast<usize>(count), wide, text))) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    i32 w = 0, h = 0;
    std::vector<i32> cumulative;
    if (!mgr.text_extent(hdc, text, w, h, &cumulative)) {
        m.set_return(0u);
        return;
    }
    // With lpnFit only the fitting characters' extents are stored
    // ("GetTextExtentExPointW(max20)=1 fit=3 dx=9,16,19,0,0"); without it all
    // of them are ("(nofit) dx=9,16,19,22,29").
    i32 fit = 0;
    while (fit < static_cast<i32>(cumulative.size()) && cumulative[static_cast<usize>(fit)] <= max_extent) ++fit;
    if (fit_ptr != 0) (void)m.write_u32(fit_ptr, static_cast<u32>(fit));
    if (dx_ptr != 0) {
        std::vector<i32> out(cumulative.begin(), cumulative.begin() + (fit_ptr != 0 ? fit : static_cast<i32>(cumulative.size())));
        (void)write_i32s(m, dx_ptr, out);
    }
    const i32 sz[2] = {w, h};
    (void)m.memory().write_bytes(size, reinterpret_cast<const u8*>(sz), 8);
    // Measuring characters clears the last error on x64 and x86 alike
    // ("GetTextExtentExPointW ok=1 err=0"; c == 0 leaves it).
    if (count > 0) set_last_error(0);
    m.set_return(1u);
}

// GetTextExtentPointI(hdc, pgiIn, cgi, psize)
void get_text_extent_point_i(GdiManager& mgr, Marshaler& m) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr glyphs_ptr = m.arg_ptr(1);
    const i32 count = static_cast<i32>(m.arg32(2));
    const GuestAddr size = m.arg_ptr(3);
    if (!mgr.has_dc(hdc)) {  // "GetTextExtentPointI(NULL dc)=0 err=57005"
        m.set_return(0u);
        return;
    }
    if (count < 0 || size == 0 || (count > 0 && glyphs_ptr == 0)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::vector<u16> glyphs(static_cast<usize>(count));
    if (count > 0 && m.memory().read_bytes(glyphs_ptr, reinterpret_cast<u8*>(glyphs.data()), glyphs.size() * 2).is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    auto advances = mgr.glyph_advances(hdc, glyphs, nullptr);
    auto face = mgr.text_face(hdc);
    if (!advances || !face) {
        m.set_return(0u);
        return;
    }
    i32 w = 0;
    for (const i32 a : *advances) w += a + mgr.get_text_character_extra(hdc);
    // "GetTextExtentPointI(0)=1 0x0"
    const i32 sz[2] = {w, count > 0 ? face->height : 0};
    (void)m.memory().write_bytes(size, reinterpret_cast<const u8*>(sz), 8);
    m.set_return(1u);
}

// GetTextMetricsW/A(hdc, lptm)
void get_text_metrics(GdiManager& mgr, Marshaler& m, bool wide) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr tm = m.arg_ptr(1);
    // "GetTextMetricsW(NULL)=0 err=57005"; success leaves it untouched too.
    if (!mgr.has_dc(hdc) || tm == 0) {
        m.set_return(0u);
        return;
    }
    m.set_return(mgr.write_text_metrics(hdc, tm, wide) ? 1u : 0u);
}

// GetCharWidthW/A, GetCharWidth32W/A(hdc, iFirst, iLast, lpBuffer)
void get_char_width(GdiManager& mgr, Marshaler& m, bool wide) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const u32 first = m.arg32(1);
    const u32 last = m.arg32(2);
    const GuestAddr buffer = m.arg_ptr(3);
    // A NULL DC, an inverted range and a NULL buffer all fail with
    // ERROR_INVALID_PARAMETER (s3gdi_otm_probe "GetCharWidth32W(...)").
    if (!mgr.has_dc(hdc) || last < first || buffer == 0 || last - first > 0xFFFF) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    auto widths = mgr.char_advances(hdc, char_range(first, last, wide), nullptr);
    if (!widths || !write_i32s(m, buffer, *widths)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

// GetCharABCWidthsW/A(hdc, wFirst, wLast, lpABC)
void get_char_abc_widths(GdiManager& mgr, Marshaler& m, bool wide) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const u32 first = m.arg32(1);
    const u32 last = m.arg32(2);
    const GuestAddr out = m.arg_ptr(3);
    if (!mgr.has_dc(hdc)) {  // "GetCharABCWidthsW(NULL dc)=0 err=6"
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    if (last < first || out == 0 || last - first > 0xFFFF) {  // "(last<first)=0 err=87"
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::vector<std::array<i32, 3>> abc;
    auto widths = mgr.char_advances(hdc, char_range(first, last, wide), &abc);
    if (!widths) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::vector<i32> flat;
    for (const auto& v : abc) flat.insert(flat.end(), v.begin(), v.end());
    if (!write_i32s(m, out, flat)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

// GetCharABCWidthsFloatW/A(hdc, iFirst, iLast, lpABC): the device ABC
// widths as floats ("GetCharABCWidthsFloatW(j)=1 -1.000,3.000,1.000").
void get_char_abc_widths_float(GdiManager& mgr, Marshaler& m, bool wide) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const u32 first = m.arg32(1);
    const u32 last = m.arg32(2);
    const GuestAddr out = m.arg_ptr(3);
    if (!mgr.has_dc(hdc)) {  // "GetCharABCWidthsFloatW(NULL dc)=0 err=6"
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    if (last < first || out == 0 || last - first > 0xFFFF) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::vector<std::array<i32, 3>> abc;
    if (!mgr.char_advances(hdc, char_range(first, last, wide), &abc)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::vector<float> flat;
    for (const auto& v : abc) {
        for (const i32 c : v) flat.push_back(static_cast<float>(c));
    }
    if (!flat.empty() &&
        m.memory().write_bytes(out, reinterpret_cast<const u8*>(flat.data()), flat.size() * 4).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

// GetTextFaceW/A(hdc, c, lpName)
void get_text_face(GdiManager& mgr, Marshaler& m, bool wide) {
    const auto hdc = static_cast<Handle>(m.arg(0));
    const i32 count = static_cast<i32>(m.arg32(1));
    const GuestAddr out = m.arg_ptr(2);
    auto face = mgr.text_face(hdc);
    if (!face) {  // "GetTextFaceW(NULL dc)=0 err=57005"
        m.set_return(0u);
        return;
    }
    std::u16string name = face->reported_face;
    if (!wide) {
        std::vector<u8> bytes(name.size() * 2 + 1);
        const auto r = codepage::wide_to_multi_byte(codepage::kCpAcp, 0, reinterpret_cast<const u16*>(name.data()),
                                                    name.size(), bytes.data(), bytes.size(), nullptr, false);
        std::string ansi(bytes.begin(), bytes.begin() + r.count);
        // W and A count differently (s3gdi_text_probe / s3gdi_otm_probe):
        // a NULL buffer answers length + 1; A copies c - 1 characters and
        // returns what it copied without the NUL (cch4 -> 3, cch64 -> 5);
        // c <= 0 with a buffer is ERROR_INVALID_PARAMETER.
        if (out == 0) {
            m.set_return(static_cast<u32>(ansi.size() + 1));
            return;
        }
        if (count <= 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
        const usize n = std::min<usize>(ansi.size(), static_cast<usize>(count - 1));
        std::vector<u8> copy(ansi.begin(), ansi.begin() + static_cast<std::ptrdiff_t>(n));
        copy.push_back(0);
        (void)m.memory().write_bytes(out, copy.data(), copy.size());
        m.set_return(static_cast<u32>(n));
        return;
    }
    // W: a NULL buffer answers length + 1, c <= 0 returns 0 with the error
    // untouched, otherwise c - 1 characters and a NUL are copied and the
    // count includes the NUL (cch4 -> 4, full -> 6 for "Arial").
    if (out == 0) {
        m.set_return(static_cast<u32>(name.size() + 1));
        return;
    }
    if (count <= 0) {
        m.set_return(0u);
        return;
    }
    const usize n = std::min<usize>(name.size(), static_cast<usize>(count - 1));
    std::u16string copy = name.substr(0, n);
    copy.push_back(u'\0');
    (void)m.memory().write_bytes(out, reinterpret_cast<const u8*>(copy.data()), copy.size() * 2);
    m.set_return(static_cast<u32>(n + 1));
}

// SetTextCharacterExtra / GetTextCharacterExtra: the previous extra, or
// 0x80000000 for a bad DC with the error untouched.
void set_text_character_extra(GdiManager& mgr, Marshaler& m) {
    m.set_return(static_cast<u32>(mgr.set_text_character_extra(static_cast<Handle>(m.arg(0)),
                                                               static_cast<i32>(m.arg32(1)))));
}

void get_text_character_extra(GdiManager& mgr, Marshaler& m) {
    m.set_return(static_cast<u32>(mgr.get_text_character_extra(static_cast<Handle>(m.arg(0)))));
}

// SetTextAlign / GetTextAlign: previous mode, or GDI_ERROR for a bad DC with
// the error untouched ("GetTextAlign(NULL)=4294967295 err=57005").
void set_text_align(GdiManager& mgr, Marshaler& m) {
    m.set_return(mgr.set_text_align_mode(static_cast<Handle>(m.arg(0)), m.arg32(1)));
}

void get_text_align(GdiManager& mgr, Marshaler& m) {
    m.set_return(mgr.get_text_align_mode(static_cast<Handle>(m.arg(0))));
}

// AddFontMemResourceEx(pFileView, cjSize, pvResrved, pNumFonts)
void add_font_mem_resource_ex(GdiManager& mgr, Marshaler& m) {
    const GuestAddr data = m.arg_ptr(0);
    const u32 size = m.arg32(1);
    const GuestAddr num_fonts = m.arg_ptr(3);
    // NULL data or count pointer: NULL with ERROR_INVALID_PARAMETER; bytes
    // that hold no font: NULL with the error and *pNumFonts untouched
    // (s3gdi_otm_probe "AddFontMemResourceEx(...)").
    if (data == 0 || size == 0 || num_fonts == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::vector<u8> bytes(size);
    if (m.memory().read_bytes(data, bytes.data(), bytes.size()).is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    u32 count = 0;
    const Handle h = mgr.add_font_mem_resource(bytes, &count);
    if (h != 0) (void)m.write_u32(num_fonts, count);
    m.set_return(static_cast<u64>(h));
}

// RemoveFontMemResourceEx(h): NULL -> ERROR_INVALID_PARAMETER; a handle that
// was never returned -> FALSE, error untouched.
void remove_font_mem_resource_ex(GdiManager& mgr, Marshaler& m) {
    const auto h = static_cast<Handle>(m.arg(0));
    if (h == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    m.set_return(mgr.remove_font_mem_resource(h) ? 1u : 0u);
}

// AddFontResource[Ex]W/A(name[, fl, pdv]) -> fonts added, 0 with
// ERROR_FILE_NOT_FOUND for a missing file and ERROR_INVALID_PARAMETER for
// flags other than FR_PRIVATE | FR_NOT_ENUM.
void add_font_resource(GdiManager& mgr, Marshaler& m, bool wide, bool ex) {
    constexpr u32 FR_PRIVATE = 0x10, FR_NOT_ENUM = 0x20;
    const GuestAddr name_ptr = m.arg_ptr(0);
    const u32 flags = ex ? m.arg32(1) : 0u;
    if (name_ptr == 0 || (flags & ~(FR_PRIVATE | FR_NOT_ENUM)) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    // Several files joined by '|' name one font (Type 1 .pfm|.pfb).
    const std::u16string name = read_cstring(m, name_ptr, wide);
    const std::u16string first = name.substr(0, name.find(u'|'));
    u32 error = 0;
    const i32 n = mgr.add_font_resource(first, &error);
    if (n == 0 && error != 0) set_last_error(error);
    m.set_return(static_cast<u32>(n));
}

void remove_font_resource(GdiManager& mgr, Marshaler& m, bool wide, bool ex) {
    (void)ex;
    const GuestAddr name_ptr = m.arg_ptr(0);
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const std::u16string name = read_cstring(m, name_ptr, wide);
    u32 error = 0;
    const bool ok = mgr.remove_font_resource(name.substr(0, name.find(u'|')), &error);
    if (!ok && error != 0) set_last_error(error);
    m.set_return(ok ? 1u : 0u);
}

}  // namespace vitreo::winapi::gdi_text_exports
