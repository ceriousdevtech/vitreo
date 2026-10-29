// SPDX-License-Identifier: LGPL-2.1-only

/// @file frame_drawing.cpp
/// @brief DrawEdge, DrawFrameControl, DrawFocusRect and InvertRect.
///
/// Every pixel here was measured on Windows 11 in the interactive session
/// (tools/oracle_probe/s3gdi_userpaint_probe.c, a 32bpp DIB section):
///  - DrawEdge draws each ring as a top row and left column in its top-left
///    colour, then a bottom row and right column in its bottom-right colour
///    over them, and moves inwards on the sides it drew; raised outer is
///    3DLIGHT/3DDKSHADOW, raised inner 3DHILIGHT/3DSHADOW, sunken outer
///    3DSHADOW/3DHILIGHT, sunken inner 3DDKSHADOW/3DLIGHT; BF_SOFT swaps the
///    two top-left colours; BF_FLAT is 3DSHADOW then 3DFACE; BF_MIDDLE fills
///    with 3DFACE; BF_ADJUST returns the rectangle inside the rings.
///  - DrawFocusRect inverts the colour of every border pixel whose
///    x + y is odd, the reserved byte untouched.
///  - InvertRect inverts all 32 bits (DSTINVERT).
///  - DrawFrameControl's glyphs (check mark, radio circle, close cross,
///    scroll arrow, menu check) are the 13x13 Marlett images Windows draws at
///    the default check-box size; other sizes scale them.

#include "winapi/user32/painting/frame_drawing.hpp"
#include "winapi/user32/painting/sys_colors.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <algorithm>
#include <cstdio>

namespace vitreo::winapi::user_draw {

namespace {

constexpr u32 COLOR_WINDOW = 5, COLOR_WINDOWFRAME = 6, COLOR_WINDOWTEXT = 8, COLOR_3DFACE = 15,
              COLOR_3DSHADOW = 16, COLOR_GRAYTEXT = 17, COLOR_3DHILIGHT = 20, COLOR_3DDKSHADOW = 21,
              COLOR_3DLIGHT = 22;

u32 sys(u32 index) { return sys_colors::get(index); }

bool fill(GdiManager& gdi, Handle hdc, i32 l, i32 t, i32 r, i32 b, u32 color) {
    if (r <= l || b <= t) return true;
    return gdi.fill_rect_gdi(hdc, l, t, r, b, color);
}

/// One ring of an edge; returns the rectangle inside it.
void ring(GdiManager& gdi, Handle hdc, Rect& rc, u32 flags, u32 tl, u32 br) {
    constexpr u32 BF_LEFT = 1, BF_TOP = 2, BF_RIGHT = 4, BF_BOTTOM = 8;
    if (flags & BF_TOP) fill(gdi, hdc, rc.left, rc.top, rc.right, rc.top + 1, tl);
    if (flags & BF_LEFT) fill(gdi, hdc, rc.left, rc.top, rc.left + 1, rc.bottom, tl);
    if (flags & BF_BOTTOM) fill(gdi, hdc, rc.left, rc.bottom - 1, rc.right, rc.bottom, br);
    if (flags & BF_RIGHT) fill(gdi, hdc, rc.right - 1, rc.top, rc.right, rc.bottom, br);
    if (flags & BF_LEFT) ++rc.left;
    if (flags & BF_TOP) ++rc.top;
    if (flags & BF_RIGHT) --rc.right;
    if (flags & BF_BOTTOM) --rc.bottom;
}

/// A 13x13 glyph grid painted into @p rc, scaled when the box is another
/// size. Characters map to colours through @p colour_of ('.' = skip).
template <typename ColourOf>
void glyph(GdiManager& gdi, Handle hdc, const Rect& rc, const char* const rows[13], ColourOf colour_of) {
    const i32 w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    for (i32 y = 0; y < h; ++y) {
        const i32 gy = std::min(12, y * 13 / h);
        for (i32 x = 0; x < w; ++x) {
            const i32 gx = std::min(12, x * 13 / w);
            const char c = rows[gy][gx];
            if (c == '.') continue;
            (void)fill(gdi, hdc, rc.left + x, rc.top + y, rc.left + x + 1, rc.top + y + 1, colour_of(c));
        }
    }
}

// s3gdi_userpaint_probe "dfc checked": the check mark inside the box.
constexpr const char* kCheck[13] = {
    ".............", ".............", ".............", ".........x...", "........xx...",
    "...x...xxx...", "...xx.xxx....", "...xxxxx.....", "....xxx......", ".....x.......",
    ".............", ".............", ".............",
};
// "dfc menucheck": the same mark one pixel left and down.
constexpr const char* kMenuCheck[13] = {
    ".............", ".............", ".............", ".............", "........x....",
    ".......xx....", "..x...xxx....", "..xx.xxx.....", "..xxxxx......", "...xxx.......",
    "....x........", ".............", ".............",
};
// "dfc close": the cross.
constexpr const char* kClose[13] = {
    ".............", ".............", "..xx....xx...", "...xx..xx....", "....xxxx.....",
    ".....xx......", "....xxxx.....", "...xx..xx....", "..xx....xx...", ".............",
    ".............", ".............", ".............",
};
// "dfc scrollup": the up arrow.
constexpr const char* kArrowUp[13] = {
    ".............", ".............", ".............", ".............", ".............",
    "......x......", ".....xxx.....", "....xxxxx....", ".............", ".............",
    ".............", ".............", ".............",
};
// "dfc radio": a = 3DSHADOW, b = 3DDKSHADOW, h = 3DHILIGHT, l = 3DLIGHT,
// w = the window-coloured interior.
constexpr const char* kRadio[13] = {
    ".....aaaa....", "...aabbbbaa..", "..abbwwwwbbh.", "..abwwwwwwlh.", ".abwwwwwwwwlh",
    ".abwwwwwwwwlh", ".abwwwwwwwwlh", ".abwwwwwwwwlh", "..abwwwwwwlh.", "..allwwwwllh.",
    "...hhllllhh..", ".....hhhh....", ".............",
};

/// Rotate the up arrow for the other directions.
void arrow(GdiManager& gdi, Handle hdc, const Rect& rc, int direction, u32 colour) {
    char rows[13][14];
    for (int y = 0; y < 13; ++y) {
        for (int x = 0; x < 13; ++x) {
            int sx = x, sy = y;
            switch (direction) {
            case 1: sx = x; sy = 12 - y; break;        // down
            case 2: sx = 12 - y; sy = x; break;        // left
            case 3: sx = y; sy = 12 - x; break;        // right
            default: break;
            }
            rows[y][x] = kArrowUp[sy][sx];
        }
        rows[y][13] = 0;
    }
    const char* ptrs[13];
    for (int i = 0; i < 13; ++i) ptrs[i] = rows[i];
    glyph(gdi, hdc, rc, ptrs, [&](char) { return colour; });
}

}  // namespace

bool draw_edge(GdiManager& gdi, Handle hdc, Rect& rc, u32 edge, u32 flags) {
    constexpr u32 BDR_RAISEDOUTER = 1, BDR_SUNKENOUTER = 2, BDR_RAISEDINNER = 4, BDR_SUNKENINNER = 8;
    constexpr u32 BF_DIAGONAL = 0x10, BF_MIDDLE = 0x800, BF_SOFT = 0x1000, BF_ADJUST = 0x2000,
                  BF_FLAT = 0x4000, BF_MONO = 0x8000;
    if (!gdi.has_dc(hdc)) return false;  // "DrawEdge(NULL dc)=0 err=57005"
    if (flags & BF_DIAGONAL) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!DrawEdge: BF_DIAGONAL\n");
        }
        return false;
    }
    Rect r = rc;
    const u32 outer = edge & (BDR_RAISEDOUTER | BDR_SUNKENOUTER);
    const u32 inner = edge & (BDR_RAISEDINNER | BDR_SUNKENINNER);
    const bool soft = (flags & BF_SOFT) != 0;
    if (flags & BF_MONO) {
        if (outer) ring(gdi, hdc, r, flags, sys(COLOR_WINDOWFRAME), sys(COLOR_WINDOWFRAME));
        if (inner) ring(gdi, hdc, r, flags, sys(COLOR_WINDOW), sys(COLOR_WINDOW));
    } else if (flags & BF_FLAT) {
        // "drawedge raised flat": 3DSHADOW ring, then 3DFACE.
        if (outer) ring(gdi, hdc, r, flags, sys(COLOR_3DSHADOW), sys(COLOR_3DSHADOW));
        if (inner) ring(gdi, hdc, r, flags, sys(COLOR_3DFACE), sys(COLOR_3DFACE));
    } else {
        u32 otl = 0, obr = 0, itl = 0, ibr = 0;
        if (outer & BDR_RAISEDOUTER) {
            otl = sys(COLOR_3DLIGHT);
            obr = sys(COLOR_3DDKSHADOW);
        } else if (outer) {
            otl = sys(COLOR_3DSHADOW);
            obr = sys(COLOR_3DHILIGHT);
        }
        if (inner & BDR_RAISEDINNER) {
            itl = sys(COLOR_3DHILIGHT);
            ibr = sys(COLOR_3DSHADOW);
        } else if (inner) {
            itl = sys(COLOR_3DDKSHADOW);
            ibr = sys(COLOR_3DLIGHT);
        }
        if (soft && outer && inner) std::swap(otl, itl);  // "dfc push": 3DHILIGHT outside
        if (outer) ring(gdi, hdc, r, flags, otl, obr);
        if (inner) ring(gdi, hdc, r, flags, itl, ibr);
    }
    if (flags & BF_MIDDLE) {
        fill(gdi, hdc, r.left, r.top, r.right, r.bottom, sys((flags & BF_MONO) ? COLOR_WINDOW : COLOR_3DFACE));
    }
    if (flags & BF_ADJUST) rc = r;
    return true;
}

bool draw_frame_control(GdiManager& gdi, Handle hdc, Rect& rc, u32 type, u32 state) {
    constexpr u32 DFC_CAPTION = 1, DFC_MENU = 2, DFC_SCROLL = 3, DFC_BUTTON = 4;
    constexpr u32 DFCS_BUTTONCHECK = 0, DFCS_BUTTONRADIO = 4, DFCS_BUTTON3STATE = 8, DFCS_BUTTONPUSH = 0x10;
    constexpr u32 DFCS_INACTIVE = 0x100, DFCS_PUSHED = 0x200, DFCS_CHECKED = 0x400, DFCS_FLAT = 0x4000;
    constexpr u32 EDGE_RAISED = 5, EDGE_SUNKEN = 10;
    constexpr u32 BF_RECT = 15, BF_MIDDLE = 0x800, BF_SOFT = 0x1000, BF_ADJUST = 0x2000, BF_FLAT = 0x4000;
    if (!gdi.has_dc(hdc)) return false;
    auto unimplemented = [&](const char* what) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!DrawFrameControl: %s (type %u state 0x%x)\n", what,
                     type, state);
        return false;
    };
    const u32 kind = state & 0xFF;
    const bool inactive = (state & DFCS_INACTIVE) != 0;
    const u32 glyph_colour = sys(inactive ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT);
    auto button_face = [&](Rect inner, bool soft) {
        u32 flags = BF_RECT | BF_MIDDLE | BF_ADJUST | (soft ? BF_SOFT : 0u);
        if (state & DFCS_FLAT) flags |= BF_FLAT;
        (void)draw_edge(gdi, hdc, inner, (state & DFCS_PUSHED) ? EDGE_SUNKEN : EDGE_RAISED, flags);
        return inner;
    };
    // Checkbox and radio glyphs are the host's own (AppKit) controls, so an
    // owner-drawn check list looks like the native checkboxes beside it. The
    // classic drawing below remains for a host without a renderer.
    auto host_glyph = [&](bool radio) {
        auto* bridge = gdi.cocoa_bridge();
        const i32 w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (bridge == nullptr || w <= 0 || h <= 0) return false;
        windowing::BitmapHost::ButtonGlyph glyph;
        glyph.radio = radio;
        glyph.checked = (state & DFCS_CHECKED) != 0;
        glyph.mixed = !radio && glyph.checked && (kind & 0x0F) == DFCS_BUTTON3STATE;
        glyph.pushed = (state & DFCS_PUSHED) != 0;
        glyph.enabled = !inactive;
        std::vector<u8> rgba;
        if (!static_cast<windowing::BitmapHost&>(*bridge).render_button_glyph(glyph, w, h, rgba)) {
            return false;
        }
        GdiManager::IconDraw icon;
        icon.width = static_cast<u32>(w);
        icon.height = static_cast<u32>(h);
        icon.color = rgba.data();
        icon.alpha = true;
        std::vector<u8> mask(static_cast<usize>(w) * h * 2, 0);
        icon.mask = mask.data();
        constexpr u32 DI_NORMAL = 3;
        return gdi.draw_icon(hdc, rc.left, rc.top, w, h, icon, DI_NORMAL, 0, nullptr);
    };
    switch (type) {
    case DFC_BUTTON:
        if ((kind & DFCS_BUTTONPUSH) == 0 &&
            ((kind & 0x0F) == DFCS_BUTTONRADIO || (kind & 0x0F) == DFCS_BUTTONCHECK ||
             (kind & 0x0F) == DFCS_BUTTON3STATE) &&
            host_glyph((kind & 0x0F) == DFCS_BUTTONRADIO)) {
            return true;
        }
        if (kind & DFCS_BUTTONPUSH) {
            (void)button_face(rc, true);
            return true;
        }
        if ((kind & 0x0F) == DFCS_BUTTONRADIO) {
            if (state & DFCS_CHECKED) return unimplemented("radio dot");
            glyph(gdi, hdc, rc, kRadio, [&](char c) {
                switch (c) {
                case 'a': return sys(COLOR_3DSHADOW);
                case 'b': return sys(COLOR_3DDKSHADOW);
                case 'h': return sys(COLOR_3DHILIGHT);
                case 'l': return sys(COLOR_3DLIGHT);
                default: return sys((inactive || (state & DFCS_PUSHED)) ? COLOR_3DFACE : COLOR_WINDOW);
                }
            });
            return true;
        }
        if ((kind & 0x0F) == DFCS_BUTTONCHECK || (kind & 0x0F) == DFCS_BUTTON3STATE) {
            // "dfc check": a sunken edge round the window-coloured box.
            Rect inner = rc;
            (void)draw_edge(gdi, hdc, inner, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
            fill(gdi, hdc, inner.left, inner.top, inner.right, inner.bottom,
                 sys((inactive || (state & DFCS_PUSHED)) ? COLOR_3DFACE : COLOR_WINDOW));
            if (state & DFCS_CHECKED) glyph(gdi, hdc, rc, kCheck, [&](char) { return glyph_colour; });
            return true;
        }
        return unimplemented("button kind");
    case DFC_CAPTION:
        if (kind == 0) {  // DFCS_CAPTIONCLOSE: a soft button and the cross
            (void)button_face(rc, true);
            glyph(gdi, hdc, rc, kClose, [&](char) { return glyph_colour; });
            return true;
        }
        return unimplemented("caption glyph");
    case DFC_SCROLL:
        if (kind <= 3) {  // DFCS_SCROLLUP/DOWN/LEFT/RIGHT: a raised button and the arrow
            (void)button_face(rc, false);
            arrow(gdi, hdc, rc, static_cast<int>(kind), glyph_colour);
            return true;
        }
        return unimplemented("scroll glyph");
    case DFC_MENU:
        if (kind == 1) {  // DFCS_MENUCHECK: black on white, a mask image
            fill(gdi, hdc, rc.left, rc.top, rc.right, rc.bottom, 0xFFFFFF);
            glyph(gdi, hdc, rc, kMenuCheck, [&](char) { return 0x000000u; });
            return true;
        }
        return unimplemented("menu glyph");
    default:
        return unimplemented("control type");
    }
}

bool draw_focus_rect(GdiManager& gdi, Handle hdc, const Rect& rc) {
    if (!gdi.has_dc(hdc)) return false;  // "DrawFocusRect(NULL dc)=0"
    return gdi.xor_border_dots(hdc, rc.left, rc.top, rc.right, rc.bottom);
}

bool invert_rect(GdiManager& gdi, Handle hdc, const Rect& rc) {
    if (!gdi.has_dc(hdc)) return false;  // "InvertRect(NULL dc)=0"
    constexpr u32 DSTINVERT = 0x00550009;
    return gdi.pat_blt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, DSTINVERT);
}

}  // namespace vitreo::winapi::user_draw
