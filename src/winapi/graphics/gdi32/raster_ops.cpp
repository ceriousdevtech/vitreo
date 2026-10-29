// SPDX-License-Identifier: LGPL-2.1-only

/// @file raster_ops.cpp
/// @brief The raster core every pixel-writing GDI export shares: logical to
/// device mapping, clipping, the ternary/binary raster operations, brush
/// realisation (solid, DC_BRUSH, hatch, pattern) and colour <-> monochrome
/// conversion, plus the exports built directly on them - PatBlt, BitBlt,
/// StretchBlt, AlphaBlend, TransparentBlt, SetPixel/GetPixel, LineTo and the
/// solid fills.
///
/// Measured on Windows 11 by tools/oracle_probe/s3gdi_blit_probe:
///  - GDI drawing leaves 0 in the reserved byte of a 32bpp memory bitmap;
///    BitBlt ROPs run on all 32 bits (SRCCOPY copies the byte, WHITENESS
///    writes ffffffff, DSTINVERT inverts it);
///  - only bits 16-23 of a ROP code matter;
///  - a monochrome source paints the destination DC's text colour for 0 and
///    its background colour for 1; a colour source lands on a monochrome
///    destination as 1 exactly where it equals the source DC's background;
///  - a solid brush on a monochrome surface is white only if it equals the
///    DC's background colour, while pens and SetPixel take the nearer of
///    black and white;
///  - hatch brushes paint the DC background colour between the lines even in
///    TRANSPARENT mode, from the 8x8 patterns in kHatch;
///  - StretchBlt samples pixel centres; BLACKONWHITE/WHITEONBLACK AND/OR the
///    block [floor(i*s/d), floor((i+1)*s/d)) when shrinking.
/// A window's redirection surface keeps the reserved byte opaque instead, as
/// its presenter composites that byte (SoftBitmap::window_surface).

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"

#include "core/memory/guest_memory.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/model/hwnd_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vitreo::winapi {

namespace {

/// 8x8 hatch patterns, bit 7 = leftmost pixel (hatch probe, all six styles).
constexpr u8 kHatch[6][8] = {
    {0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00},  // HS_HORIZONTAL
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},  // HS_VERTICAL
    {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01},  // HS_FDIAGONAL
    {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},  // HS_BDIAGONAL
    {0x08, 0x08, 0x08, 0xFF, 0x08, 0x08, 0x08, 0x08},  // HS_CROSS
    {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81},  // HS_DIAGCROSS
};

constexpr u32 kWhite = 0x00FFFFFFu;

/// A pixel as one little-endian word: R | G<<8 | B<<16 | reserved<<24. A
/// COLORREF (0x00BBGGRR) is the same word with a zero reserved byte.
[[nodiscard]] inline u32 load_px(const u8* p) noexcept {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
inline void store_px(u8* p, u32 v) noexcept { std::memcpy(p, &v, 4); }

[[nodiscard]] inline i32 positive_mod(i32 v, i32 m) noexcept {
    const i32 r = v % m;
    return r < 0 ? r + m : r;
}

/// The boolean function a ROP index encodes over pattern, source and
/// destination words: bit k of the index is the result for the minterm
/// (P<<2 | S<<1 | D) == k.
[[nodiscard]] u32 rop3_words(u8 index, u32 p, u32 s, u32 d) noexcept {
    switch (index) {
    case 0x00: return 0;
    case 0x33: return ~s;
    case 0x44: return s & ~d;
    case 0x55: return ~d;
    case 0x5A: return p ^ d;
    case 0x66: return s ^ d;
    case 0x88: return s & d;
    case 0xAA: return d;
    case 0xBB: return ~s | d;
    case 0xC0: return p & s;
    case 0xCC: return s;
    case 0xEE: return s | d;
    case 0xF0: return p;
    case 0xFF: return ~0u;
    default: break;
    }
    u32 r = 0;
    for (u32 k = 0; k < 8; ++k) {
        if (((index >> k) & 1u) == 0) continue;
        r |= ((k & 4u) ? p : ~p) & ((k & 2u) ? s : ~s) & ((k & 1u) ? d : ~d);
    }
    return r;
}

/// SetROP2 modes R2_BLACK (1) .. R2_WHITE (16): mode - 1 is the truth table
/// over (P<<1 | D).
[[nodiscard]] u32 rop2_words(i32 mode, u32 p, u32 d) noexcept {
    if (mode < 1 || mode > 16) mode = 13;  // R2_COPYPEN
    const u32 code = static_cast<u32>(mode - 1);
    u32 r = 0;
    for (u32 k = 0; k < 4; ++k) {
        if (((code >> k) & 1u) == 0) continue;
        r |= ((k & 2u) ? p : ~p) & ((k & 1u) ? d : ~d);
    }
    return r;
}

/// Nearer of black and white, the rule for pens and SetPixel on a
/// monochrome surface (s3gdi_blit_probe "mono draw": 808080 -> white,
/// 7f7f7f -> black).
[[nodiscard]] inline u32 nearest_mono(u32 colorref) noexcept {
    const i32 r = static_cast<i32>(colorref & 0xFF);
    const i32 g = static_cast<i32>((colorref >> 8) & 0xFF);
    const i32 b = static_cast<i32>((colorref >> 16) & 0xFF);
    const i32 to_black = r * r + g * g + b * b;
    const i32 to_white = (255 - r) * (255 - r) + (255 - g) * (255 - g) + (255 - b) * (255 - b);
    return to_white < to_black ? kWhite : 0u;
}

/// The sampling loop StretchBlt, BitBlt, StretchDIBits and
/// SetDIBitsToDevice share. A negative extent covers (origin + extent,
/// origin] and mirrors; enlarging samples pixel centres, and so does
/// shrinking in COLORONCOLOR, while BLACKONWHITE/WHITEONBLACK AND/OR and
/// HALFTONE averages the block [floor(i*s/d), floor((i+1)*s/d)).
/// @p sample returns false where the source has no pixel.
template <typename Target, typename Sample, typename Pattern, typename Store, typename Load>
void stretch_loop(Target& t, i32 mode, u8 index, i32 dxo, i32 dyo, i32 dw, i32 dh, i32 sxo,
                  i32 syo, i32 sw, i32 sh, Sample&& sample, Pattern&& pattern, Store&& store,
                  Load&& load, bool mono) {
    const i32 aw = std::abs(dw), ah = std::abs(dh);
    const i32 asw = std::abs(sw), ash = std::abs(sh);
    const bool shrink_x = asw > aw, shrink_y = ash > ah;
    const bool combine = (shrink_x || shrink_y) && (mode == 1 || mode == 2 || mode == 4);
    auto centre = [](i32 i, i32 s, i32 d) {
        return static_cast<i32>((static_cast<i64>(2 * i + 1) * s) / (2 * d));
    };
    for (i32 j = 0; j < ah; ++j) {
        const i32 py = dh > 0 ? dyo + j : dyo - j;
        for (i32 i = 0; i < aw; ++i) {
            const i32 px = dw > 0 ? dxo + i : dxo - i;
            if (!t.writable(px, py)) continue;
            u32 s = 0;
            if (!combine) {
                const i32 si = centre(i, asw, aw), sj = centre(j, ash, ah);
                if (!sample(sw > 0 ? sxo + si : sxo - si, sh > 0 ? syo + sj : syo - sj, s)) continue;
            } else {
                const i32 i0 = shrink_x ? static_cast<i32>(static_cast<i64>(i) * asw / aw) : centre(i, asw, aw);
                const i32 i1 = shrink_x ? static_cast<i32>(static_cast<i64>(i + 1) * asw / aw) : i0 + 1;
                const i32 j0 = shrink_y ? static_cast<i32>(static_cast<i64>(j) * ash / ah) : centre(j, ash, ah);
                const i32 j1 = shrink_y ? static_cast<i32>(static_cast<i64>(j + 1) * ash / ah) : j0 + 1;
                u32 acc = mode == 1 ? ~0u : 0u;
                u32 sum[4] = {0, 0, 0, 0};
                u32 count = 0;
                for (i32 sj = j0; sj < std::max(j1, j0 + 1); ++sj) {
                    for (i32 si = i0; si < std::max(i1, i0 + 1); ++si) {
                        u32 v = 0;
                        if (!sample(sw > 0 ? sxo + si : sxo - si, sh > 0 ? syo + sj : syo - sj, v)) continue;
                        if (mode == 1) acc &= v;
                        else if (mode == 2) acc |= v;
                        for (u32 c = 0; c < 4; ++c) sum[c] += (v >> (8 * c)) & 0xFFu;
                        ++count;
                    }
                }
                if (count == 0) continue;
                if (mode == 4) {
                    acc = 0;
                    for (u32 c = 0; c < 4; ++c) acc |= ((sum[c] + count / 2) / count) << (8 * c);
                }
                s = acc;
            }
            u32 v = rop3_words(index, pattern(px, py), s, load(px, py));
            if (mono) v = (v & 1u) ? kWhite : 0u;
            store(px, py, v);
        }
    }
}

}  // namespace

// =============================================================================
// Mapping and clipping
// =============================================================================

void GdiManager::lp_to_dp(const DcState& dc, double x, double y, double& ox, double& oy) const {
    ApiLock api_lock(*this);
    if (dc.graphics_mode == 2) {  // GM_ADVANCED: world transform first
        const WorldXform& w = dc.world_xform;
        const double nx = x * w.eM11 + y * w.eM21 + w.eDx;
        const double ny = x * w.eM12 + y * w.eM22 + w.eDy;
        x = nx;
        y = ny;
    }
    // Page transform (Learn: Window and Viewport). MM_TEXT is 1:1; the
    // metric modes are fixed extents at 96 dpi with y growing upward.
    double wex = 1, wey = 1, vex = 1, vey = 1;
    switch (dc.map_mode) {
    case 2: wex = wey = 254; vex = 96; vey = -96; break;     // MM_LOMETRIC
    case 3: wex = wey = 2540; vex = 96; vey = -96; break;    // MM_HIMETRIC
    case 4: wex = wey = 100; vex = 96; vey = -96; break;     // MM_LOENGLISH
    case 5: wex = wey = 1000; vex = 96; vey = -96; break;    // MM_HIENGLISH
    case 6: wex = wey = 1440; vex = 96; vey = -96; break;    // MM_TWIPS
    case 7: case 8:                                          // ISOTROPIC/ANISOTROPIC
        wex = dc.window_ext_x != 0 ? dc.window_ext_x : 1;
        wey = dc.window_ext_y != 0 ? dc.window_ext_y : 1;
        vex = dc.viewport_ext_x;
        vey = dc.viewport_ext_y;
        break;
    default: break;
    }
    ox = (x - dc.window_org_x) * vex / wex + dc.viewport_org_x;
    oy = (y - dc.window_org_y) * vey / wey + dc.viewport_org_y;
}

void GdiManager::lp_to_dp_point(const DcState& dc, i32& x, i32& y) const {
    ApiLock api_lock(*this);
    double dx = 0, dy = 0;
    lp_to_dp(dc, x, y, dx, dy);
    x = static_cast<i32>(std::lround(dx));
    y = static_cast<i32>(std::lround(dy));
}

void GdiManager::lp_to_dp_rect(const DcState& dc, i32& l, i32& t, i32& r, i32& b) const {
    ApiLock api_lock(*this);
    i32 x0 = l, y0 = t, x1 = r, y1 = b;
    lp_to_dp_point(dc, x0, y0);
    lp_to_dp_point(dc, x1, y1);
    l = std::min(x0, x1);
    r = std::max(x0, x1);
    t = std::min(y0, y1);
    b = std::max(y0, y1);
}

bool GdiManager::raster_target(Handle hdc, RasterTarget& out) {
    ApiLock api_lock(*this);
    const auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || !it->second.bitmap) return false;
    out.hdc = hdc;
    out.dc = &it->second;
    out.surface = it->second.bitmap.get();
    out.mono = out.surface->bpp == 1;
    out.alpha = out.surface->draw_alpha();
    out.dib = m_dib_sections.contains(it->second.selected_bitmap_handle)
                  ? it->second.selected_bitmap_handle
                  : Handle{0};
    if (out.dib != 0) sync_dib_from_guest(out.dib);
    return true;
}

void GdiManager::finish_raster(const RasterTarget& t) {
    ApiLock api_lock(*this);
    if (t.surface) t.surface->dirty = true;
    if (t.dib != 0) sync_dib_to_guest(t.dib);
    if (t.surface && t.surface->bpp != 1 && t.surface->bpp != 32 && t.dib == 0) {
        quantize_ddb_rows(*t.surface, 0, t.surface->height);
    }
}

bool GdiManager::raster_writable(const RasterTarget& t, i32 x, i32 y) const {
    ApiLock api_lock(*this);
    return t.surface->in_bounds(x, y) && t.dc->clip_rect.contains(x, y);
}

void GdiManager::raster_store(const RasterTarget& t, i32 x, i32 y, u32 value) const {
    ApiLock api_lock(*this);
    u8* p = t.surface->pixel_at(x, y);
    if (t.mono) {
        value = (value & kWhite) ? kWhite : 0u;
    } else if (t.surface->window_surface) {
        value |= 0xFF000000u;
    }
    store_px(p, value);
}

// =============================================================================
// Brushes
// =============================================================================

bool GdiManager::brush_fill_color(Handle hdc, Handle hbrush, u32& color) const {
    ApiLock api_lock(*this);
    if (const auto dc_brush = m_stock_objects.find(DC_BRUSH_ID);
        dc_brush != m_stock_objects.end() && dc_brush->second == hbrush) {
        const auto dc = m_dc_states.find(hdc);
        color = dc != m_dc_states.end() ? dc->second.dc_brush_color : kWhite;
        return true;
    }
    if (const auto info = m_brush_info.find(hbrush); info != m_brush_info.end()) {
        if (info->second.style != 0) return false;  // only BS_SOLID
        color = info->second.color;
        return true;
    }
    if (const auto solid = m_brush_colors.find(hbrush); solid != m_brush_colors.end()) {
        color = solid->second;
        return true;
    }
    if (m_system_brush_color) {
        if (const auto sys = system_brush_color(hbrush)) {
            color = *sys;
            return true;
        }
    }
    return false;
}

bool GdiManager::brush_pattern_word(const RasterTarget& t, Handle hbrush, i32 x, i32 y,
                                    u32& out) const {
    ApiLock api_lock(*this);
    const DcState& dc = *t.dc;
    auto realize = [&](u32 colorref) {
        // A brush on a monochrome surface is white when it is white or equals
        // the background colour, black otherwise (s3gdi_blit_probe "mono
        // draw": c0c0c0 is black with a 999999 background, 808080 white with
        // an 808080 one).
        if (t.mono) {
            const u32 c = colorref & kWhite;
            return (c == kWhite || c == (dc.bk_color & kWhite)) ? kWhite : 0u;
        }
        return colorref & kWhite;
    };
    u32 solid = 0;
    if (brush_fill_color(t.hdc, hbrush, solid)) {
        out = realize(solid);
        return true;
    }
    const auto info = m_brush_info.find(hbrush);
    if (info != m_brush_info.end() && info->second.style == 2) {  // BS_HATCHED
        const u32 hatch = static_cast<u32>(info->second.hatch);
        if (hatch > 5) return false;
        const i32 px = positive_mod(x - dc.brush_org_x, 8);
        const i32 py = positive_mod(y - dc.brush_org_y, 8);
        const bool line = (kHatch[hatch][py] >> (7 - px)) & 1u;
        out = realize(line ? info->second.color : dc.bk_color);
        return true;
    }
    if (const auto pat = m_brush_patterns.find(hbrush);
        pat != m_brush_patterns.end() && pat->second && pat->second->width > 0 &&
        pat->second->height > 0) {
        const SoftBitmap& p = *pat->second;
        const i32 px = positive_mod(x - dc.brush_org_x, p.width);
        const i32 py = positive_mod(y - dc.brush_org_y, p.height);
        u32 v = load_px(p.pixel_at(px, py));
        if (p.bpp == 1) {
            // A monochrome pattern takes the DC's colours: 1 = background,
            // 0 = text (hatch probe "monopattern").
            v = (v & kWhite) ? (dc.bk_color & kWhite) : (dc.text_color & kWhite);
            out = t.mono ? realize(v) : v;
        } else {
            out = t.mono ? realize(v & kWhite) : (v & kWhite);
        }
        return true;
    }
    return false;
}

bool GdiManager::fill_rect_with_brush(Handle hdc, i32 left, i32 top, i32 right, i32 bottom,
                                      Handle brush) {
    ApiLock api_lock(*this);
    const bool known = m_brush_info.contains(brush) || m_brush_colors.contains(brush) ||
                       m_brush_patterns.contains(brush) ||
                       (m_stock_objects.contains(DC_BRUSH_ID) &&
                        m_stock_objects.at(DC_BRUSH_ID) == brush);
    if (!known) return false;
    RasterTarget t;
    if (!raster_target(hdc, t)) return true;
    lp_to_dp_rect(*t.dc, left, top, right, bottom);
    bool any = false;
    for (i32 y = std::max(top, 0); y < std::min(bottom, t.surface->height); ++y) {
        for (i32 x = std::max(left, 0); x < std::min(right, t.surface->width); ++x) {
            if (!t.dc->clip_rect.contains(x, y)) continue;
            u32 p = 0;
            if (!brush_pattern_word(t, brush, x, y, p)) return true;  // hollow
            raster_store(t, x, y, p);
            any = true;
        }
    }
    if (any) finish_raster(t);
    return true;
}

bool GdiManager::xor_border_dots(Handle hdc, i32 left, i32 top, i32 right, i32 bottom) {
    ApiLock api_lock(*this);
    RasterTarget t;
    if (!raster_target(hdc, t)) return m_dc_states.contains(hdc);
    lp_to_dp_rect(*t.dc, left, top, right, bottom);
    if (right <= left || bottom <= top) return true;
    auto flip = [&](i32 x, i32 y) {
        if (((x + y) & 1) == 0 || !raster_writable(t, x, y)) return;
        const u32 d = load_px(t.surface->pixel_at(x, y));
        raster_store(t, x, y, (d ^ kWhite) & 0x00FFFFFFu);
    };
    for (i32 x = left; x < right; ++x) {
        flip(x, top);
        if (bottom - 1 != top) flip(x, bottom - 1);
    }
    for (i32 y = top + 1; y < bottom - 1; ++y) {
        flip(left, y);
        if (right - 1 != left) flip(right - 1, y);
    }
    finish_raster(t);
    return true;
}

bool GdiManager::fill_rect_gdi(Handle hdc, i32 left, i32 top, i32 right, i32 bottom,
                               u32 colorref) {
    ApiLock api_lock(*this);
    RasterTarget t;
    if (!raster_target(hdc, t)) return false;
    lp_to_dp_rect(*t.dc, left, top, right, bottom);
    const u32 c = colorref & kWhite;
    const u32 value = t.mono ? ((c == kWhite || c == (t.dc->bk_color & kWhite)) ? kWhite : 0u) : c;
    for (i32 y = std::max(top, 0); y < std::min(bottom, t.surface->height); ++y) {
        for (i32 x = std::max(left, 0); x < std::min(right, t.surface->width); ++x) {
            if (t.dc->clip_rect.contains(x, y)) raster_store(t, x, y, value);
        }
    }
    finish_raster(t);
    return true;
}

template <typename Sample, typename Pattern>
void GdiManager::stretch_core(const RasterTarget& t, i32 mode, u8 index, i32 dxo, i32 dyo,
                              i32 dw, i32 dh, i32 sxo, i32 syo, i32 sw, i32 sh, Sample&& sample,
                              Pattern&& pattern) {
    ApiLock api_lock(*this);
    struct View {
        const GdiManager* self;
        const RasterTarget* t;
        bool writable(i32 x, i32 y) const { return self->raster_writable(*t, x, y); }
    } view{this, &t};
    stretch_loop(view, mode, index, dxo, dyo, dw, dh, sxo, syo, sw, sh, sample, pattern,
                 [&](i32 x, i32 y, u32 v) { raster_store(t, x, y, v); },
                 [&](i32 x, i32 y) { return load_px(t.surface->pixel_at(x, y)); }, t.mono);
}

// =============================================================================
// PatBlt / BitBlt / StretchBlt
// =============================================================================

bool GdiManager::pat_blt(Handle hdc, i32 x, i32 y, i32 cx, i32 cy, u32 rop) {
    ApiLock api_lock(*this);
    const u8 index = static_cast<u8>(rop >> 16);
    // PatBlt takes only ROPs that do not read a source (s3gdi_blit_probe
    // "PatBlt SRCCOPY(invalid)": FALSE, last error untouched).
    if (((index >> 2) & 0x33u) != (index & 0x33u)) return false;
    RasterTarget t;
    if (!raster_target(hdc, t)) return m_dc_states.contains(hdc);
    i32 l = x, tp = y, r = x + cx, b = y + cy;
    lp_to_dp_rect(*t.dc, l, tp, r, b);
    const Handle brush = t.dc->selected_brush;
    for (i32 py = std::max(tp, 0); py < std::min(b, t.surface->height); ++py) {
        for (i32 px = std::max(l, 0); px < std::min(r, t.surface->width); ++px) {
            if (!t.dc->clip_rect.contains(px, py)) continue;
            u32 p = 0;
            const bool has_pattern = brush_pattern_word(t, brush, px, py, p);
            // A hollow brush contributes nothing to a pattern ROP.
            if (!has_pattern && ((index >> 4) & 0x0Fu) != (index & 0x0Fu)) continue;
            const u32 d = load_px(t.surface->pixel_at(px, py));
            raster_store(t, px, py, rop3_words(index, p, 0, d));
        }
    }
    finish_raster(t);
    return true;
}

std::shared_ptr<SoftBitmap> GdiManager::resolve_screen_source(DcState& dc, i32& sx, i32& sy,
                                                              i32 blit_cx, i32 blit_cy) {
    ApiLock api_lock(*this);
    if (dc.bitmap) return dc.bitmap;
    if (!dc.screen_dc || !m_hwnd_mgr) return nullptr;
    // GetDC(NULL) has no surface of its own. A screen-space blit reads the
    // window covering the rectangle: the tightest-fitting window that holds
    // all of it.
    windowing::HWND best_hwnd = windowing::NULL_HWND;
    i32 best_score = std::numeric_limits<i32>::max();
    i32 best_ox = 0, best_oy = 0;
    const i32 want_area = (blit_cx > 0 && blit_cy > 0) ? (blit_cx * blit_cy) : 0;
    m_hwnd_mgr->for_each_hwnd([&](windowing::HWND hwnd) {
        const auto* win = m_hwnd_mgr->find(hwnd);
        if (!win) return;
        i32 ox = 0, oy = 0;
        for (windowing::HWND cur = hwnd;
             cur != windowing::HWND_DESKTOP && cur != windowing::NULL_HWND;) {
            const auto* w = m_hwnd_mgr->find(cur);
            if (!w) break;
            ox += w->rect.left;
            oy += w->rect.top;
            cur = w->parent;
        }
        i32 cw = win->client_rect.right - win->client_rect.left;
        i32 ch = win->client_rect.bottom - win->client_rect.top;
        if (cw <= 0 || ch <= 0) {
            cw = win->rect.right - win->rect.left;
            ch = win->rect.bottom - win->rect.top;
        }
        if (cw <= 0 || ch <= 0) return;
        if (blit_cx > 0 && blit_cy > 0) {
            if (sx < ox || sy < oy || sx + blit_cx > ox + cw || sy + blit_cy > oy + ch) return;
        } else if (sx < ox || sy < oy || sx >= ox + cw || sy >= oy + ch) {
            return;
        }
        const i32 area = cw * ch;
        const i32 score = want_area > 0 ? std::abs(area - want_area) : area;
        if (score < best_score) {
            best_score = score;
            best_hwnd = hwnd;
            best_ox = ox;
            best_oy = oy;
        }
    });
    if (best_hwnd == windowing::NULL_HWND) return nullptr;
    sx -= best_ox;
    sy -= best_oy;
    if (m_cocoa_bridge) {
        std::vector<u8> rgba;
        i32 cw = 0, ch = 0;
        const auto bridge = m_cocoa_bridge;
        bool captured = false;
        {
            ApiUnlock unlocked(*this);
            captured = bridge->capture_window_rgba(best_hwnd, rgba, cw, ch);
        }
        if (captured && cw > 0 && ch > 0 &&
            rgba.size() >= static_cast<size_t>(cw) * ch * 4u) {
            auto snap = std::make_shared<SoftBitmap>();
            snap->width = cw;
            snap->height = ch;
            snap->pixels = std::move(rgba);
            snap->window_surface = true;
            snap->dirty = false;
            return snap;
        }
    }
    const auto fb = m_window_framebuffers.find(static_cast<u64>(best_hwnd));
    return fb != m_window_framebuffers.end() ? fb->second : nullptr;
}

bool GdiManager::bit_blt(Handle hdcDest, i32 x, i32 y, i32 cx, i32 cy, Handle hdcSrc,
                         i32 x1, i32 y1, u32 rop) {
    ApiLock api_lock(*this);
    const u8 index = static_cast<u8>(rop >> 16);
    const bool uses_source = ((index >> 2) & 0x33u) != (index & 0x33u);
    if (!uses_source) return pat_blt(hdcDest, x, y, cx, cy, rop);
    if (!m_dc_states.contains(hdcDest) || !m_dc_states.contains(hdcSrc)) return false;
    // A negative extent draws nothing and succeeds (s3gdi_blit_probe
    // "BitBlt negative width"); StretchBlt mirrors instead.
    if (cx <= 0 || cy <= 0) return true;
    return stretch_blt(hdcDest, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, rop);
}

bool GdiManager::stretch_blt(Handle hdcDest, i32 xd, i32 yd, i32 wd, i32 hd, Handle hdcSrc,
                             i32 xs, i32 ys, i32 ws, i32 hs, u32 rop) {
    ApiLock api_lock(*this);
    const u8 index = static_cast<u8>(rop >> 16);
    const bool uses_source = ((index >> 2) & 0x33u) != (index & 0x33u);
    if (!uses_source) return pat_blt(hdcDest, xd, yd, wd, hd, rop);
    auto src_it = m_dc_states.find(hdcSrc);
    if (!m_dc_states.contains(hdcDest) || src_it == m_dc_states.end()) return false;
    if (wd == 0 || hd == 0 || ws == 0 || hs == 0) return true;

    RasterTarget t;
    if (!raster_target(hdcDest, t)) return true;
    DcState& sdc = src_it->second;

    // Map both rectangles through their DC's transform (MM_TEXT extents).
    double dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
    lp_to_dp(*t.dc, xd, yd, dx0, dy0);
    lp_to_dp(*t.dc, static_cast<double>(xd) + wd, static_cast<double>(yd) + hd, dx1, dy1);
    double sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    lp_to_dp(sdc, xs, ys, sx0, sy0);
    lp_to_dp(sdc, static_cast<double>(xs) + ws, static_cast<double>(ys) + hs, sx1, sy1);
    const i32 dxo = static_cast<i32>(std::lround(dx0));
    const i32 dyo = static_cast<i32>(std::lround(dy0));
    const i32 dw = static_cast<i32>(std::lround(dx1)) - dxo;
    const i32 dh = static_cast<i32>(std::lround(dy1)) - dyo;
    i32 sxo = static_cast<i32>(std::lround(sx0));
    i32 syo = static_cast<i32>(std::lround(sy0));
    const i32 sw = static_cast<i32>(std::lround(sx1)) - sxo;
    const i32 sh = static_cast<i32>(std::lround(sy1)) - syo;
    if (dw == 0 || dh == 0 || sw == 0 || sh == 0) return true;

    std::shared_ptr<SoftBitmap> src = resolve_screen_source(
        sdc, sxo, syo, std::abs(sw), std::abs(sh));
    if (!src) return true;
    if (m_dib_sections.contains(sdc.selected_bitmap_handle) && sdc.bitmap == src) {
        sync_dib_from_guest(sdc.selected_bitmap_handle);
    }
    // Overlapping self-blits read a snapshot.
    std::vector<u8> snapshot;
    const u8* src_pixels = src->pixels.data();
    if (src.get() == t.surface) {
        snapshot = src->pixels;
        src_pixels = snapshot.data();
    }
    const bool src_mono = src->bpp == 1;
    const u32 src_bk = sdc.bk_color & kWhite;
    const bool src_window = src->window_surface;
    auto source_word = [&](i32 sx, i32 sy) -> u32 {
        const u32 v = load_px(src_pixels + (static_cast<usize>(sy) * src->width + sx) * 4u);
        if (src_mono && !t.mono) {
            // 0 -> destination text colour, 1 -> destination background.
            return (v & kWhite) ? (t.dc->bk_color & kWhite) : (t.dc->text_color & kWhite);
        }
        if (!src_mono && t.mono) {
            const bool match = src_window ? (v & kWhite) == src_bk : v == src_bk;
            return match ? kWhite : 0u;
        }
        return v;
    };

    const Handle brush = t.dc->selected_brush;
    const bool uses_pattern = ((index >> 4) & 0x0Fu) != (index & 0x0Fu);
    stretch_core(t, t.dc->stretch_mode, index, dxo, dyo, dw, dh, sxo, syo, sw, sh,
                 [&](i32 sx, i32 sy, u32& out) {
                     if (sx < 0 || sy < 0 || sx >= src->width || sy >= src->height) return false;
                     out = source_word(sx, sy);
                     return true;
                 },
                 [&](i32 px, i32 py) {
                     u32 p = 0;
                     if (uses_pattern) (void)brush_pattern_word(t, brush, px, py, p);
                     return p;
                 });
    finish_raster(t);
    return true;
}

// =============================================================================
// AlphaBlend / TransparentBlt
// =============================================================================

u32 GdiManager::alpha_blend_checked(Handle hdcDest, i32 xDest, i32 yDest, i32 wDest, i32 hDest,
                                    Handle hdcSrc, i32 xSrc, i32 ySrc, i32 wSrc, i32 hSrc,
                                    u8 blend_op, u8 const_alpha, u8 alpha_format, bool strict) {
    ApiLock api_lock(*this);
    constexpr u32 kInvalidParameter = 87;
    constexpr u32 kFailKeepError = 0xFFFFFFFFu;
    // s3gdi_blit_probe "AB ...": parameter rejections are FALSE/87 and
    // BlendFlags is not checked; a NULL DC fails with the last error
    // untouched and zero extents succeed doing nothing (s3gdi misc probe).
    auto src_it = m_dc_states.find(hdcSrc);
    if (!m_dc_states.contains(hdcDest) || src_it == m_dc_states.end()) return kFailKeepError;
    if (blend_op != 0 || alpha_format > 1) return kInvalidParameter;
    if (wDest < 0 || hDest < 0 || wSrc < 0 || hSrc < 0) return kInvalidParameter;
    RasterTarget t;
    if (!raster_target(hdcDest, t)) return kFailKeepError;
    DcState& sdc = src_it->second;
    std::shared_ptr<SoftBitmap> src = sdc.bitmap;
    if (!src) return kFailKeepError;
    i32 sx0 = xSrc, sy0 = ySrc;
    lp_to_dp_point(sdc, sx0, sy0);
    if (strict && (sx0 < 0 || sy0 < 0 || sx0 + wSrc > src->width || sy0 + hSrc > src->height)) {
        return kInvalidParameter;
    }
    if (strict && src.get() == t.surface) {
        // Same surface: overlapping rectangles are rejected.
        i32 dx0 = xDest, dy0 = yDest;
        lp_to_dp_point(*t.dc, dx0, dy0);
        const bool overlap = dx0 < sx0 + wSrc && sx0 < dx0 + wDest &&
                             dy0 < sy0 + hSrc && sy0 < dy0 + hDest;
        if (overlap) return kInvalidParameter;
    }
    if (wDest == 0 || hDest == 0 || wSrc == 0 || hSrc == 0) return 0;
    if (m_dib_sections.contains(sdc.selected_bitmap_handle)) {
        sync_dib_from_guest(sdc.selected_bitmap_handle);
    }
    i32 dxo = xDest, dyo = yDest;
    lp_to_dp_point(*t.dc, dxo, dyo);
    const bool per_pixel = alpha_format == 1;
    const u32 ca = const_alpha;
    auto round255 = [](u32 v) { return (v + 127u) / 255u; };
    for (i32 j = 0; j < hDest; ++j) {
        const i32 py = dyo + j;
        const i32 sy = sy0 + static_cast<i32>((static_cast<i64>(2 * j + 1) * hSrc) / (2 * hDest));
        for (i32 i = 0; i < wDest; ++i) {
            const i32 px = dxo + i;
            if (!raster_writable(t, px, py)) continue;
            const i32 sx = sx0 + static_cast<i32>((static_cast<i64>(2 * i + 1) * wSrc) / (2 * wDest));
            if (!src->in_bounds(sx, sy)) continue;
            const u32 s = load_px(src->pixel_at(sx, sy));
            const u32 d = load_px(t.surface->pixel_at(px, py));
            u32 out = 0;
            if (per_pixel) {
                // Premultiplied source over destination: s' = s*ca, then
                // d = s' + d*(255 - a')/255, each rounded (AB sweep).
                u32 sp[4];
                for (u32 c = 0; c < 4; ++c) sp[c] = round255(((s >> (8 * c)) & 0xFFu) * ca);
                const u32 inv = 255u - sp[3];
                for (u32 c = 0; c < 4; ++c) {
                    const u32 v = sp[c] + round255(((d >> (8 * c)) & 0xFFu) * inv);
                    out |= std::min(v, 255u) << (8 * c);
                }
            } else {
                // Constant alpha only: every byte blends (AB const sweep).
                for (u32 c = 0; c < 4; ++c) {
                    const u32 v = round255(((s >> (8 * c)) & 0xFFu) * ca +
                                           ((d >> (8 * c)) & 0xFFu) * (255u - ca));
                    out |= v << (8 * c);
                }
            }
            // The blended fourth byte is kept even on a window surface: a
            // layered window presents it as per-pixel alpha.
            store_px(t.surface->pixel_at(px, py), out);
        }
    }
    finish_raster(t);
    return 0;
}

bool GdiManager::alpha_blend(Handle hdcDest, i32 xDest, i32 yDest, i32 wDest, i32 hDest,
                             Handle hdcSrc, i32 xSrc, i32 ySrc, i32 wSrc, i32 hSrc,
                             u8 src_const_alpha, bool src_has_alpha) {
    ApiLock api_lock(*this);
    // The internal entry (UpdateLayeredWindow's composite) clips a source
    // rectangle that overruns the source instead of rejecting it.
    return alpha_blend_checked(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc,
                               hSrc, 0, src_const_alpha, src_has_alpha ? 1 : 0, false) == 0;
}

bool GdiManager::compose_layered_window(Handle hdcWin, Handle hdcSrc, i32 xSrc, i32 ySrc,
                                        i32 width, i32 height, u8 const_alpha,
                                        bool per_pixel, bool opaque) {
    ApiLock api_lock(*this);
    auto src_it = m_dc_states.find(hdcSrc);
    if (src_it == m_dc_states.end() || !src_it->second.bitmap) return false;
    RasterTarget t;
    if (!raster_target(hdcWin, t)) return false;
    DcState& sdc = src_it->second;
    if (m_dib_sections.contains(sdc.selected_bitmap_handle)) {
        sync_dib_from_guest(sdc.selected_bitmap_handle);
    }
    const SoftBitmap& src = *sdc.bitmap;
    i32 sx0 = xSrc, sy0 = ySrc;
    lp_to_dp_point(sdc, sx0, sy0);
    const u32 ca = const_alpha;
    auto round255 = [](u32 v) { return (v + 127u) / 255u; };
    for (i32 py = 0; py < t.surface->height; ++py) {
        for (i32 px = 0; px < t.surface->width; ++px) {
            const i32 sx = sx0 + px, sy = sy0 + py;
            u32 out = 0;
            if (px < width && py < height && src.in_bounds(sx, sy)) {
                const u32 s = load_px(src.pixel_at(sx, sy));
                if (opaque) {
                    out = (s & 0x00FFFFFFu) | 0xFF000000u;
                } else {
                    for (u32 c = 0; c < 3; ++c)
                        out |= round255(((s >> (8 * c)) & 0xFFu) * ca) << (8 * c);
                    const u32 a = per_pixel ? round255((s >> 24) * ca) : ca;
                    out |= a << 24;
                }
            }
            store_px(t.surface->pixel_at(px, py), out);
        }
    }
    finish_raster(t);
    return true;
}

bool GdiManager::transparent_blt(Handle hdcDest, i32 xd, i32 yd, i32 wd, i32 hd,
                                 Handle hdcSrc, i32 xs, i32 ys, i32 ws, i32 hs, u32 key) {
    ApiLock api_lock(*this);
    // Negative or zero extents fail with the last error untouched
    // (s3gdi_blit_probe "TB negative", misc probe "TB zero").
    if (wd <= 0 || hd <= 0 || ws <= 0 || hs <= 0) return false;
    auto src_it = m_dc_states.find(hdcSrc);
    if (src_it == m_dc_states.end() || !src_it->second.bitmap) return false;
    RasterTarget t;
    if (!raster_target(hdcDest, t)) return false;
    DcState& sdc = src_it->second;
    if (m_dib_sections.contains(sdc.selected_bitmap_handle)) {
        sync_dib_from_guest(sdc.selected_bitmap_handle);
    }
    const SoftBitmap& src = *sdc.bitmap;
    i32 dxo = xd, dyo = yd, sxo = xs, syo = ys;
    lp_to_dp_point(*t.dc, dxo, dyo);
    lp_to_dp_point(sdc, sxo, syo);
    for (i32 j = 0; j < hd; ++j) {
        const i32 sy = syo + static_cast<i32>((static_cast<i64>(2 * j + 1) * hs) / (2 * hd));
        for (i32 i = 0; i < wd; ++i) {
            const i32 px = dxo + i, py = dyo + j;
            if (!raster_writable(t, px, py)) continue;
            const i32 sx = sxo + static_cast<i32>((static_cast<i64>(2 * i + 1) * ws) / (2 * wd));
            if (sx < 0 || sy < 0 || sx >= src.width || sy >= src.height) continue;
            const u32 v = load_px(src.pixel_at(sx, sy)) & kWhite;
            // The key compares colour only, and what is copied loses the
            // reserved byte ("TB key 563412").
            if (v == (key & kWhite)) continue;
            raster_store(t, px, py, v);
        }
    }
    finish_raster(t);
    return true;
}

u32 GdiManager::gradient_fill(Handle hdc, GuestAddr vertices, u32 vertex_count, GuestAddr mesh,
                              u32 mesh_count, u32 mode) {
    ApiLock api_lock(*this);
    constexpr u32 kInvalidParameter = 87;
    // GRADIENT_FILL_RECT_H 0, _RECT_V 1, _TRIANGLE 2; anything else is 87
    // ("GF bad mode"). A mesh index past the vertices fails with the last
    // error untouched ("GF bad index").
    if (mode > 2) return kInvalidParameter;
    if (!m_guest_memory || (vertex_count != 0 && vertices == 0) || (mesh_count != 0 && mesh == 0)) {
        return kInvalidParameter;
    }
    struct Vertex { i32 x, y; u32 c[4]; };  // c: R, G, B, A as COLOR16
    std::vector<Vertex> v(vertex_count);
    for (u32 i = 0; i < vertex_count; ++i) {
        u8 raw[16];
        if (m_guest_memory->read_bytes(vertices + i * 16u, raw, 16).is_err()) return kInvalidParameter;
        auto le16 = [&](usize o) { return static_cast<u32>(raw[o]) | (static_cast<u32>(raw[o + 1]) << 8); };
        v[i].x = static_cast<i32>(le16(0) | (le16(2) << 16));
        v[i].y = static_cast<i32>(le16(4) | (le16(6) << 16));
        for (u32 c = 0; c < 4; ++c) v[i].c[c] = le16(8 + c * 2);
    }
    const u32 per = mode == 2 ? 3u : 2u;
    std::vector<u32> idx(static_cast<usize>(mesh_count) * per);
    for (usize i = 0; i < idx.size(); ++i) {
        const auto r = m_guest_memory->read_u32(mesh + static_cast<GuestAddr>(i) * 4u);
        if (r.is_err()) return kInvalidParameter;
        idx[i] = r.value();
        if (idx[i] >= vertex_count) return 0xFFFFFFFFu;  // failure, error untouched
    }
    RasterTarget t;
    if (!raster_target(hdc, t)) return kInvalidParameter;
    auto store = [&](i32 x, i32 y, const double* c) {
        i32 dx = x, dy = y;
        lp_to_dp_point(*t.dc, dx, dy);
        if (!raster_writable(t, dx, dy)) return;
        u32 word = 0;
        for (u32 k = 0; k < 4; ++k) {
            const u32 v16 = static_cast<u32>(std::clamp(c[k], 0.0, 65535.0));
            word |= (v16 >> 8) << (8 * k);
        }
        raster_store(t, dx, dy, word);
    };
    for (u32 m = 0; m < mesh_count; ++m) {
        if (mode != 2) {
            // Colours run from the upper-left vertex to the lower-right one
            // along x (H) or y (V), in 16 bits, then keep the high byte
            // (s3gdi_blit_probe "GF rect H/V").
            const Vertex& a = v[idx[m * 2]];
            const Vertex& b = v[idx[m * 2 + 1]];
            const i32 x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
            const i32 y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
            const bool horizontal = mode == 0;
            const Vertex& lo = horizontal ? (a.x <= b.x ? a : b) : (a.y <= b.y ? a : b);
            const Vertex& hi = &lo == &a ? b : a;
            const i64 span = horizontal ? (x1 - x0) : (y1 - y0);
            for (i32 y = y0; y < y1; ++y) {
                for (i32 x = x0; x < x1; ++x) {
                    const i64 pos = horizontal ? (x - x0) : (y - y0);
                    double c[4];
                    for (u32 k = 0; k < 4; ++k) {
                        const i64 delta = static_cast<i64>(hi.c[k]) - static_cast<i64>(lo.c[k]);
                        c[k] = static_cast<double>(static_cast<i64>(lo.c[k]) +
                                                   (span ? delta * pos / span : 0));
                    }
                    store(x, y, c);
                }
            }
            continue;
        }
        // Triangle: barycentric colour at each pixel corner inside the
        // triangle, top-left fill rule.
        const Vertex& a = v[idx[m * 3]];
        const Vertex& b = v[idx[m * 3 + 1]];
        const Vertex& c3 = v[idx[m * 3 + 2]];
        const double area = static_cast<double>(b.x - a.x) * (c3.y - a.y) -
                            static_cast<double>(b.y - a.y) * (c3.x - a.x);
        if (area == 0) continue;
        const i32 minx = std::min({a.x, b.x, c3.x}), maxx = std::max({a.x, b.x, c3.x});
        const i32 miny = std::min({a.y, b.y, c3.y}), maxy = std::max({a.y, b.y, c3.y});
        const Vertex* pts[3] = {&a, &b, &c3};
        auto edge = [](const Vertex& p, const Vertex& q, double x, double y) {
            return static_cast<double>(q.x - p.x) * (y - p.y) - static_cast<double>(q.y - p.y) * (x - p.x);
        };
        auto top_left = [&](const Vertex& p, const Vertex& q) {
            // With positive area (clockwise on screen) a left edge runs
            // upward and a top edge rightward: (0,4)->(0,0) is kept and
            // (0,0)->(8,4) is not (s3gdi_blit_probe "GF triangle").
            const i32 ex = q.x - p.x, ey = q.y - p.y;
            return (area > 0) ? (ey < 0 || (ey == 0 && ex > 0)) : (ey > 0 || (ey == 0 && ex < 0));
        };
        for (i32 y = miny; y < maxy; ++y) {
            for (i32 x = minx; x < maxx; ++x) {
                double w[3];
                bool inside = true;
                for (u32 e = 0; e < 3 && inside; ++e) {
                    const Vertex& p = *pts[(e + 1) % 3];
                    const Vertex& q = *pts[(e + 2) % 3];
                    w[e] = edge(p, q, x, y) / area;
                    if (w[e] < 0 || (w[e] == 0 && !top_left(p, q))) inside = false;
                }
                if (!inside) continue;
                double c[4];
                for (u32 k = 0; k < 4; ++k) {
                    c[k] = w[0] * a.c[k] + w[1] * b.c[k] + w[2] * c3.c[k];
                }
                store(x, y, c);
            }
        }
    }
    finish_raster(t);
    return 0;
}

// =============================================================================
// StretchDIBits / SetDIBitsToDevice
// =============================================================================

bool GdiManager::blit_dib(Handle hdc, i32 dxo, i32 dyo, i32 dw, i32 dh, i32 xsrc, i32 ysrc,
                          i32 sw, i32 sh, GuestAddr bits, const gdi::DibHeader& hdr,
                          const gdi::PixelFormat& format, u32 first_scan, u32 scan_count,
                          u8 index) {
    ApiLock api_lock(*this);
    RasterTarget t;
    if (!raster_target(hdc, t)) return false;
    const i32 width = hdr.width;
    const i32 height = hdr.abs_height();
    const u32 stride = hdr.stride();
    // DIB source coordinates have their origin at the lower left whatever the
    // DIB's orientation (s3gdi_blit_probe "SDIB td src(1,1)"): turn the
    // rectangle into top-down rows of a virtual top-down image.
    const i32 top = height - ysrc - sh;
    // Decode each needed buffer row once.
    std::vector<std::vector<u8>> rows(static_cast<usize>(height));
    std::vector<u8> packed(stride);
    auto row_of = [&](i32 y_top) -> const u8* {
        if (y_top < 0 || y_top >= height) return nullptr;
        const u32 scan = static_cast<u32>(height - 1 - y_top);  // from the bottom
        if (scan < first_scan || scan >= first_scan + scan_count) return nullptr;
        auto& decoded = rows[static_cast<usize>(y_top)];
        if (decoded.empty()) {
            // The buffer holds scans first..first+count-1, bottom-up or, for
            // a top-down DIB, top row first.
            const u32 r = hdr.top_down() ? first_scan + scan_count - 1 - scan : scan - first_scan;
            if (m_guest_memory->read_bytes(bits + static_cast<GuestAddr>(r) * stride, packed.data(),
                                           stride).is_err()) {
                return nullptr;
            }
            decoded.resize(static_cast<usize>(width) * 4u);
            gdi::decode_row(format, packed.data(), decoded.data(), width);
        }
        return decoded.data();
    };
    const u32 bk = t.dc->bk_color & kWhite;
    const bool keeps_fourth = hdr.bpp == 32 && hdr.compression == gdi::kBiRgb;
    const Handle brush = t.dc->selected_brush;
    const bool uses_pattern = ((index >> 4) & 0x0Fu) != (index & 0x0Fu);
    stretch_core(t, t.dc->stretch_mode, index, dxo, dyo, dw, dh, xsrc, top, sw, sh,
                 [&](i32 sx, i32 sy, u32& out) {
                     if (sx < 0 || sx >= width) return false;
                     const u8* row = row_of(sy);
                     if (!row) return false;
                     out = load_px(row + static_cast<usize>(sx) * 4u);
                     if (t.mono) {
                         const bool match = (out & kWhite) == bk && (!keeps_fourth || (out >> 24) == 0);
                         out = match ? kWhite : 0u;
                     }
                     return true;
                 },
                 [&](i32 px, i32 py) {
                     u32 p = 0;
                     if (uses_pattern) (void)brush_pattern_word(t, brush, px, py, p);
                     return p;
                 });
    finish_raster(t);
    return true;
}

namespace {

/// Grow a window framebuffer to cover a blit. A peer process's GetDC can see
/// a client size that is stale against the swapchain it presents (a GPU
/// process paints the browser window with StretchDIBits); the presenter crops
/// back to the client size, so this changes nothing a Win32 caller observes.
void grow_window_surface(SoftBitmap& surface, i32 right, i32 bottom) {
    if (right <= surface.width && bottom <= surface.height) return;
    const i32 w = std::max(surface.width, right);
    const i32 h = std::max(surface.height, bottom);
    SoftBitmap grown;
    grown.child_window = surface.child_window;
    grown.resize_window(w, h);
    for (i32 row = 0; row < surface.height; ++row) {
        std::memcpy(grown.pixel_at(0, row), surface.pixel_at(0, row),
                    static_cast<size_t>(surface.width) * 4u);
    }
    surface = std::move(grown);
}

}  // namespace

i32 GdiManager::stretch_dib_bits(Marshaler& /*m*/, Handle hdcDest, i32 xDest, i32 yDest,
                                 i32 wDest, i32 hDest, i32 xSrc, i32 ySrc, i32 wSrc, i32 hSrc,
                                 GuestAddr lpBits, GuestAddr lpBMI, u32 colorUsage, u32 dwRop) {
    ApiLock api_lock(*this);
    // Learn (StretchDIBits): the number of scan lines copied, 0 on failure;
    // Windows answers the DIB's height (s3gdi_blit_probe "SDIB ... r=2").
    if (!m_guest_memory || lpBits == 0 || colorUsage > gdi::kDibPalColors) return 0;
    auto it = m_dc_states.find(hdcDest);
    if (it == m_dc_states.end() || !it->second.bitmap) return 0;
    gdi::DibHeader hdr;
    if (!gdi::read_dib_header(*m_guest_memory, lpBMI, hdr)) return 0;
    if (hdr.compression == gdi::kBiJpeg || hdr.compression == gdi::kBiPng ||
        hdr.compression == gdi::kBiRle8 || hdr.compression == gdi::kBiRle4) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED gdi32!StretchDIBits: compression %u\n",
                         hdr.compression);
        }
        return 0;
    }
    if (!gdi::dib_is_decodable(hdr) || hdr.width <= 0 || hdr.height == 0) return 0;
    gdi::PixelFormat format;
    if (!gdi::dib_pixel_format(*m_guest_memory, hdr, colorUsage, dc_logical_palette(hdcDest),
                               format)) {
        return 0;
    }
    if (wDest == 0 || hDest == 0 || wSrc == 0 || hSrc == 0) return 0;
    DcState& dc = it->second;
    i32 dxo = xDest, dyo = yDest, dx1 = xDest + wDest, dy1 = yDest + hDest;
    lp_to_dp_point(dc, dxo, dyo);
    lp_to_dp_point(dc, dx1, dy1);
    if (dc.associated_hwnd != 0 && dc.bitmap->window_surface) {
        grow_window_surface(*dc.bitmap, std::max(dxo, dx1), std::max(dyo, dy1));
        m_window_client_size[dc.associated_hwnd] = {dc.bitmap->width, dc.bitmap->height};
    }
    const u8 index = static_cast<u8>(dwRop >> 16);
    if (!blit_dib(hdcDest, dxo, dyo, dx1 - dxo, dy1 - dyo, xSrc, ySrc, wSrc, hSrc, lpBits, hdr,
                  format, 0, static_cast<u32>(hdr.abs_height()), index)) {
        return 0;
    }
    if (dc.associated_hwnd != 0) flush_window(dc.associated_hwnd);
    return hdr.abs_height();
}

i32 GdiManager::set_dib_bits_to_device(Handle hdc, i32 xDest, i32 yDest, u32 w, u32 h, i32 xSrc,
                                       i32 ySrc, u32 startScan, u32 cLines, GuestAddr bits_addr,
                                       GuestAddr bmi_addr, u32 colorUse) {
    ApiLock api_lock(*this);
    if (!m_guest_memory || bits_addr == 0 || colorUse > gdi::kDibPalColors) return 0;
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || !it->second.bitmap) return 0;
    gdi::DibHeader hdr;
    if (!gdi::read_dib_header(*m_guest_memory, bmi_addr, hdr) || !gdi::dib_is_decodable(hdr) ||
        hdr.width <= 0 || hdr.height == 0) {
        return 0;
    }
    gdi::PixelFormat format;
    if (!gdi::dib_pixel_format(*m_guest_memory, hdr, colorUse, dc_logical_palette(hdc), format)) {
        return 0;
    }
    const u32 height = static_cast<u32>(hdr.abs_height());
    if (startScan >= height) return 0;
    // Learn (SetDIBitsToDevice): the number of scan lines set - the lines
    // the buffer supplies, whether or not the source rectangle reaches them
    // (s3gdi_blit_probe "SDTD bu ysrc1 r=2", "SDTD bu lines5 r=2").
    const u32 lines = std::min(cLines, height - startScan);
    if (lines == 0) return 0;
    DcState& dc = it->second;
    i32 dxo = xDest, dyo = yDest;
    lp_to_dp_point(dc, dxo, dyo);
    const i32 iw = static_cast<i32>(w), ih = static_cast<i32>(h);
    if (dc.associated_hwnd != 0 && dc.bitmap->window_surface) {
        grow_window_surface(*dc.bitmap, dxo + iw, dyo + ih);
        m_window_client_size[dc.associated_hwnd] = {dc.bitmap->width, dc.bitmap->height};
    }
    if (iw > 0 && ih > 0) {
        (void)blit_dib(hdc, dxo, dyo, iw, ih, xSrc, ySrc, iw, ih, bits_addr, hdr, format,
                       startScan, lines, 0xCC);
    }
    return static_cast<i32>(lines);
}

// =============================================================================
// Foreign rasterizers (GDI+ on an HDC)
// =============================================================================

bool GdiManager::read_device_surface(Handle hdc,
    const std::function<void(const ReadOnlyDeviceSurface&)>& read) {
    ApiLock api_lock(*this);
    RasterTarget target;
    if (!raster_target(hdc, target)) return false;
    const ReadOnlyDeviceSurface surface{target.surface->pixels.data(),
                                       target.surface->width,
                                       target.surface->height};
    read(surface);
    return true;
}

bool GdiManager::with_device_surface(Handle hdc,
                                     const std::function<void(DeviceSurface&)>& draw) {
    ApiLock api_lock(*this);
    RasterTarget t;
    if (!raster_target(hdc, t)) return false;
    DeviceSurface s;
    s.rgba = t.surface->pixels.data();
    s.width = t.surface->width;
    s.height = t.surface->height;
    s.clip = &t.dc->clip_rect;
    double ox = 0, oy = 0, xx = 0, xy = 0, yx = 0, yy = 0;
    lp_to_dp(*t.dc, 0, 0, ox, oy);
    lp_to_dp(*t.dc, 1, 0, xx, xy);
    lp_to_dp(*t.dc, 0, 1, yx, yy);
    s.xform[0] = xx - ox;
    s.xform[1] = xy - oy;
    s.xform[2] = yx - ox;
    s.xform[3] = yy - oy;
    s.xform[4] = ox;
    s.xform[5] = oy;
    s.alpha = t.alpha;
    s.mono = t.mono;
    draw(s);
    finish_raster(t);
    return true;
}

// =============================================================================
// DrawIconEx
// =============================================================================

bool GdiManager::draw_icon(Handle hdc, i32 x, i32 y, i32 cx, i32 cy, const IconDraw& icon,
                           u32 flags, Handle flicker_brush, u32* error) {
    ApiLock api_lock(*this);
    constexpr u32 DI_MASK = 0x1, DI_IMAGE = 0x2;
    RasterTarget dest;
    if (!raster_target(hdc, dest)) {
        if (error) *error = m_dc_states.contains(hdc) ? 0u : 6u;  // "DrawIconEx NULL dc": 6
        return m_dc_states.contains(hdc);
    }
    if ((flags & (DI_MASK | DI_IMAGE)) == 0 || cx <= 0 || cy <= 0 || icon.width == 0 ||
        icon.height == 0) {
        return true;  // "DrawIconEx flags 0": TRUE, nothing drawn
    }
    const bool mono = icon.color == nullptr;
    const u32 w = icon.width, h = icon.height;
    // Pixels as DrawIconEx sees them: the mask expands to black/white with
    // no regard for the DC's colours (s3gdi_icon_probe "mono ... dc-colours").
    // Colour samples pixel centres; a 1bpp plane shrinks by ANDing the
    // pixels it drops, black winning ("mono DrawIconEx 2x2" vs "opaque ...").
    const bool shrink_x = static_cast<u32>(cx) < w, shrink_y = static_cast<u32>(cy) < h;
    i32 bi = 0, bj = 0;  // destination pixel being sampled, for the AND block
    auto bit_plane = [&](u32 row0, u32 sx, u32 sy) -> u32 {
        const u32 x0 = shrink_x ? static_cast<u32>(static_cast<i64>(bi) * w / cx) : sx;
        const u32 x1 = shrink_x ? std::max(x0 + 1, static_cast<u32>(static_cast<i64>(bi + 1) * w / cx)) : sx + 1;
        const u32 y0 = shrink_y ? static_cast<u32>(static_cast<i64>(bj) * h / cy) : sy;
        const u32 y1 = shrink_y ? std::max(y0 + 1, static_cast<u32>(static_cast<i64>(bj + 1) * h / cy)) : sy + 1;
        for (u32 yy = y0; yy < y1; ++yy)
            for (u32 xx = x0; xx < x1; ++xx)
                if (!icon.mask[(row0 + yy) * w + xx]) return 0u;
        return kWhite;
    };
    auto mask_word = [&](u32 sx, u32 sy) { return bit_plane(0, sx, sy); };
    auto image_word = [&](u32 sx, u32 sy) {
        if (mono) return bit_plane(h, sx, sy);
        return load_px(icon.color + (static_cast<usize>(sy) * w + sx) * 4u);
    };
    auto render = [&](const RasterTarget& t, i32 ox, i32 oy) {
        for (i32 j = 0; j < cy; ++j) {
            const u32 sy = static_cast<u32>((static_cast<i64>(2 * j + 1) * h) / (2 * cy));
            bj = j;
            for (i32 i = 0; i < cx; ++i) {
                const i32 px = ox + i, py = oy + j;
                if (!raster_writable(t, px, py)) continue;
                bi = i;
                const u32 sx = static_cast<u32>((static_cast<i64>(2 * i + 1) * w) / (2 * cx));
                const u32 d = load_px(t.surface->pixel_at(px, py));
                u32 out = d;
                if ((flags & (DI_MASK | DI_IMAGE)) == DI_MASK) {
                    out = mask_word(sx, sy);
                } else if ((flags & (DI_MASK | DI_IMAGE)) == DI_IMAGE) {
                    out = image_word(sx, sy);
                } else if (!mono && icon.alpha) {
                    // Straight-alpha colour, premultiplied then blended over
                    // the destination; the mask is ignored (s3gdi_icon_probe
                    // "alpha DrawIconEx NORMAL", "alpha+halfmask").
                    const u32 s = image_word(sx, sy);
                    const u32 a = s >> 24;
                    out = 0;
                    for (u32 c = 0; c < 4; ++c) {
                        const u32 sc = c == 3 ? a : (((s >> (8 * c)) & 0xFFu) * a + 127u) / 255u;
                        const u32 dc = ((d >> (8 * c)) & 0xFFu) * (255u - a);
                        out |= std::min(sc + (dc + 127u) / 255u, 255u) << (8 * c);
                    }
                } else {
                    // AND the mask, then XOR the image.
                    out = (d & mask_word(sx, sy)) ^ image_word(sx, sy);
                }
                raster_store(t, px, py, out);
            }
        }
    };
    i32 dxo = x, dyo = y;
    lp_to_dp_point(*dest.dc, dxo, dyo);
    if (flicker_brush == 0) {
        render(dest, dxo, dyo);
        finish_raster(dest);
        return true;
    }
    // Flicker-free: draw into a brush-filled offscreen the size of the icon,
    // then copy it in ("alpha DrawIconEx flicker-free brush").
    SoftBitmap off;
    off.width = cx;
    off.height = cy;
    off.pixels.assign(static_cast<usize>(cx) * cy * 4u, 0);
    DcState off_state;
    off_state.bitmap = nullptr;
    RasterTarget t;
    t.dc = &off_state;
    t.surface = &off;
    t.hdc = 0;
    u32 fill = 0;
    if (brush_fill_color(hdc, flicker_brush, fill)) {
        for (usize i = 0; i < static_cast<usize>(cx) * cy; ++i) store_px(off.pixels.data() + i * 4u, fill & kWhite);
    }
    render(t, 0, 0);
    for (i32 j = 0; j < cy; ++j) {
        for (i32 i = 0; i < cx; ++i) {
            if (raster_writable(dest, dxo + i, dyo + j)) {
                raster_store(dest, dxo + i, dyo + j, load_px(off.pixel_at(i, j)));
            }
        }
    }
    finish_raster(dest);
    return true;
}

// =============================================================================
// Pixels and lines
// =============================================================================

u32 GdiManager::set_pixel(Handle hdc, i32 x, i32 y, u32 color) {
    ApiLock api_lock(*this);
    RasterTarget t;
    if (!raster_target(hdc, t)) return 0xFFFFFFFFu;
    lp_to_dp_point(*t.dc, x, y);
    if (!raster_writable(t, x, y)) return 0xFFFFFFFFu;
    // SetPixel answers the colour actually set: on a monochrome surface the
    // nearer of black and white ("mono draw ... setpixel_ret").
    const u32 value = t.mono ? nearest_mono(color) : (color & kWhite);
    raster_store(t, x, y, value);
    finish_raster(t);
    return value;
}

u32 GdiManager::get_pixel(Handle hdc, i32 x, i32 y) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || !it->second.bitmap) return 0xFFFFFFFFu;
    lp_to_dp_point(it->second, x, y);
    auto& bmp = *it->second.bitmap;
    if (!bmp.in_bounds(x, y) || !it->second.clip_rect.contains(x, y)) return 0xFFFFFFFFu;
    if (m_dib_sections.contains(it->second.selected_bitmap_handle)) {
        sync_dib_from_guest(it->second.selected_bitmap_handle);
    }
    return load_px(bmp.pixel_at(x, y)) & kWhite;
}

bool GdiManager::pen_word(const RasterTarget& t, u32& value, i32& width) const {
    ApiLock api_lock(*this);
    const DcState& dc = *t.dc;
    u32 color = 0;
    i32 style = 0;
    width = 1;
    if (const auto dc_pen = m_stock_objects.find(DC_PEN_ID);
        dc_pen != m_stock_objects.end() && dc_pen->second == dc.selected_pen) {
        color = dc.dc_pen_color;
    } else if (const auto pen = m_pen_objects.find(dc.selected_pen); pen != m_pen_objects.end()) {
        color = pen->second.color;
        style = pen->second.style & 0x0F;
        width = std::max(pen->second.width, 1);
    }
    if (style == 5) return false;  // PS_NULL
    value = t.mono ? nearest_mono(color) : (color & kWhite);
    return true;
}

void GdiManager::plot_pen(const RasterTarget& t, i32 x, i32 y, u32 pen, i32 width) {
    ApiLock api_lock(*this);
    const i32 half = (width - 1) / 2;
    for (i32 oy = -half; oy < width - half; ++oy) {
        for (i32 ox = -half; ox < width - half; ++ox) {
            const i32 px = x + ox, py = y + oy;
            if (!raster_writable(t, px, py)) continue;
            const u32 d = load_px(t.surface->pixel_at(px, py));
            raster_store(t, px, py, rop2_words(t.dc->rop2, pen, d));
        }
    }
}

bool GdiManager::line_to(Handle hdc, i32 x1, i32 y1) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    if (path_record_line(hdc, x1, y1)) return true;  // inside BeginPath
    DcState& dc = it->second;
    RasterTarget t;
    const i32 lx0 = dc.current_x, ly0 = dc.current_y;
    dc.current_x = x1;
    dc.current_y = y1;
    if (!raster_target(hdc, t)) return true;
    u32 pen = 0;
    i32 width = 1;
    if (!pen_word(t, pen, width)) return true;
    i32 x0 = lx0, y0 = ly0;
    lp_to_dp_point(dc, x0, y0);
    lp_to_dp_point(dc, x1, y1);
    draw_line_pixels(t, x0, y0, x1, y1, pen, width);
    finish_raster(t);
    return true;
}

void GdiManager::draw_line_pixels(const RasterTarget& t, i32 x0, i32 y0, i32 x1, i32 y1, u32 pen,
                                  i32 width) {
    ApiLock api_lock(*this);
    // Bresenham, the last point excluded (LineTo convention).
    const i32 dx = std::abs(x1 - x0);
    const i32 dy = -std::abs(y1 - y0);
    const i32 sx = x0 < x1 ? 1 : -1;
    const i32 sy = y0 < y1 ? 1 : -1;
    i32 err = dx + dy;
    while (x0 != x1 || y0 != y1) {
        plot_pen(t, x0, y0, pen, width);
        const i32 e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

u32 GdiManager::rop2_mix(i32 mode, u32 p, u32 d) { return rop2_words(mode, p, d); }

}  // namespace vitreo::winapi
