/// @file uxtheme_draw.cpp
/// @brief DrawThemeBackground/Edge/Text/ParentBackground, regions, hit
///        testing, part bitmaps and theme colours.
///
/// Every part is drawn by theme_render.cpp (macOS artwork) inside the
/// rectangle the guest passes. Parameter checks and their order are the
/// measured ones (uxtheme_aero_probe "api", "dtb", "dtbex", "dtt", "dttex",
/// "edge", "parentbg" lines): bad theme E_HANDLE, bad DC E_HANDLE, missing
/// rectangle or text E_POINTER, no [part, state] section E_FAIL.

#include "winapi/ui/uxtheme/uxtheme_internal.hpp"

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace vitreo::winapi::uxtheme {

using windowing::ThemeColor;

namespace {

u32 to_colorref(u32 argb) { return ((argb & 0xFF) << 16) | (argb & 0xFF00) | ((argb >> 16) & 0xFF); }

/// @p top (straight alpha) over the opaque @p bottom, as 0xRRGGBB.
u32 flatten(u32 top, u32 bottom) {
    const double a = ((top >> 24) & 0xFF) / 255.0;
    auto ch = [&](int shift) {
        const double t = (top >> shift) & 0xFF, b = (bottom >> shift) & 0xFF;
        return static_cast<u32>(std::lround(t * a + b * (1 - a))) & 0xFF;
    };
    return 0xFF000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

/// Whether @p state of @p part is a disabled state (vsstyle.h state names).
bool disabled_state(ThemeClass cls, i32 part, i32 state) {
    switch (cls) {
    case ThemeClass::Button:
        switch (part) {
        case button::BP_PUSHBUTTON: case button::BP_USERBUTTON: case button::BP_COMMANDLINK:
        case button::BP_COMMANDLINKGLYPH: return state == 4;
        case button::BP_RADIOBUTTON: case button::BP_CHECKBOX: return state > 0 && state % 4 == 0;
        case button::BP_GROUPBOX: case button::BP_PUSHBUTTONDROPDOWN: return state == 2;
        default: return part >= button::BP_RADIOBUTTON_HCDISABLED && part <= button::BP_GROUPBOX_HCDISABLED;
        }
    case ThemeClass::Edit:
        if (part == edit::EP_EDITTEXT) return state == edit::ETS_DISABLED;
        if (part == edit::EP_BACKGROUND || part == edit::EP_BACKGROUNDWITHBORDER) return state == 3;
        return state == 4;
    case ThemeClass::Combobox:
        return part == combobox::CP_TRANSPARENTBACKGROUND ? state == 3 : state == 4;
    case ThemeClass::Scrollbar:
        return part == scrollbar::SBP_ARROWBTN ? (state > 0 && state <= 16 && state % 4 == 0) : state == 4;
    case ThemeClass::Spin: case ThemeClass::Tab: case ThemeClass::Toolbar: case ThemeClass::Listview:
    case ThemeClass::Treeview:
        return state == 4;
    case ThemeClass::Trackbar: return state == 5;
    case ThemeClass::Menu:
        switch (part) {
        case menu::MENU_BARITEM: return state >= menu::MBI_DISABLED;
        case menu::MENU_POPUPCHECK: return state == 2 || state == 4;
        case menu::MENU_POPUPITEM: case menu::MENU_POPUPITEMFOCUSABLE: return state >= 3;
        case menu::MENU_POPUPSUBMENU: case menu::MENU_SYSTEMCLOSE: case menu::MENU_SYSTEMMAXIMIZE:
        case menu::MENU_SYSTEMMINIMIZE: case menu::MENU_SYSTEMRESTORE: return state == 2;
        default: return false;
        }
    case ThemeClass::Window:
        if (part >= window::WP_CAPTION && part <= window::WP_SMALLMAXCAPTION) return state == 3;
        return part >= window::WP_SYSBUTTON && part <= window::WP_VERTTHUMB && state == 4;
    default: return false;
    }
}

bool link_state(ThemeClass cls, i32 part, i32 state) {
    return cls == ThemeClass::Tooltip && (part == tooltip::TTP_STANDARD || part == tooltip::TTP_STANDARDTITLE ||
                                          part == tooltip::TTP_BALLOON || part == tooltip::TTP_BALLOONTITLE) &&
           state == 2;
}

/// The colour a part's content sits on.
u32 part_background(ThemeClass cls, const windowing::ThemePalette& p) {
    switch (cls) {
    case ThemeClass::Edit: case ThemeClass::Combobox: case ThemeClass::Listview: case ThemeClass::Treeview:
    case ThemeClass::Header:
        return p[ThemeColor::TextBackground] | 0xFF000000u;
    default:
        return p[ThemeColor::WindowBackground] | 0xFF000000u;
    }
}

}  // namespace

u32 theme_colorref(const ThemeObject& theme, const windowing::ThemePalette& p, i32 part, i32 state, i32 id) {
    const ThemeClass cls = theme.variant.record().theme_class;
    const u32 bg = part_background(cls, p);
    const bool disabled = disabled_state(cls, part, state);
    const u32 label = disabled ? p[ThemeColor::DisabledControlText]
                               : link_state(cls, part, state) ? p[ThemeColor::Link] : p[ThemeColor::Label];
    const u32 tint = theme.dark ? 0xFFFFFFu : 0x000000u;
    u32 c = bg;
    switch (id) {
    case tmt::TEXTCOLOR: case tmt::GLYPHTEXTCOLOR: case tmt::TEXTCOLORHINT: case tmt::HEADING1TEXTCOLOR:
    case tmt::BODYTEXTCOLOR:
        c = label;
        break;
    case tmt::HEADING2TEXTCOLOR: c = p[ThemeColor::SecondaryLabel]; break;
    case tmt::BORDERCOLOR: case tmt::BORDERCOLORHINT: c = (0x40u << 24) | tint; break;
    case tmt::EDGESHADOWCOLOR: c = (0x2Eu << 24) | tint; break;
    case tmt::EDGEDKSHADOWCOLOR: c = (0x50u << 24) | tint; break;
    case tmt::EDGELIGHTCOLOR: case tmt::EDGEHIGHLIGHTCOLOR: c = theme.dark ? (0x14u << 24) | 0xFFFFFFu : 0xFFFFFFFFu; break;
    case tmt::FILLCOLOR: case tmt::EDGEFILLCOLOR: case tmt::FILLCOLORHINT:
        c = cls == ThemeClass::Button ? p[ThemeColor::Control] : bg;
        break;
    case tmt::ACCENTCOLORHINT: case tmt::COLORIZATIONCOLOR: c = p[ThemeColor::Accent]; break;
    case tmt::SHADOWCOLOR: case tmt::TEXTSHADOWCOLOR: c = (0x40u << 24); break;
    default: break;  // glow, gradient, blend, transparent-key and text-border colours: the background
    }
    return to_colorref(flatten(c, bg));
}

bool render_theme_part(const UxThemeModule& mod, const ThemeObject& theme, i32 part, i32 state, i32 width,
                       i32 height, const PartDrawOptions& options, PartCanvas& out) {
    return render_part(theme.variant.record().theme_class, part, state, width, height, 1.0, mod.palette(theme.dark),
                       theme.dark, options, out);
}

namespace {

struct DrawCheck {
    std::optional<ThemeObject> theme;
    const PartStateMetrics* ps = nullptr;
    i32 rect[4]{};
    u32 hr = S_OK;
};

/// theme, DC, rectangle, then the [part, state] section.
DrawCheck draw_check(const UxThemeModule& mod, u64 htheme, u64 hdc, GuestAddr rect, i32 part, i32 state) {
    DrawCheck c;
    c.theme = mod.theme(htheme);
    if (!c.theme) { c.hr = E_HANDLE; return c; }
    if (hdc == 0 || mod.services().gdi == nullptr || !mod.services().gdi->has_dc(static_cast<Handle>(hdc))) {
        c.hr = E_HANDLE;
        return c;
    }
    if (rect == 0) { c.hr = E_POINTER; return c; }
    c.ps = find_part_state(c.theme->variant, part, state);
    if (c.ps == nullptr) { c.hr = E_FAIL; return c; }
    return c;
}

u32 draw_background(Marshaler& m, UxThemeModule& mod, u64 htheme, u64 hdc, i32 part, i32 state, GuestAddr rect_ptr,
                    const i32* clip, const PartDrawOptions& options) {
    DrawCheck c = draw_check(mod, htheme, hdc, rect_ptr, part, state);
    if (c.hr != S_OK) return c.hr;
    if (!read_rect(m, rect_ptr, c.rect)) return E_POINTER;
    if (c.rect[2] <= c.rect[0] || c.rect[3] <= c.rect[1]) return S_OK;  // empty / inverted: nothing to draw
    const ThemeObject theme = *c.theme;
    const auto palette = mod.palette(theme.dark);
    PartDrawOptions opts = options;
    opts.opaque = (c.ps->flags & kPartiallyTransparent) == 0;
    (void)paint_on_dc(*mod.services().gdi, hdc, c.rect, clip, [&](i32 w, i32 h, double scale, PartCanvas& out) {
        return render_part(theme.variant.record().theme_class, part, state, w, h, scale, palette, theme.dark,
                           opts, out);
    });
    return S_OK;
}

/// The theme font for [part, state] (TMT_FONT, or @p font_prop), selected into
/// @p hdc for the scope's lifetime.
class ScopedThemeFont {
public:
    ScopedThemeFont(GdiManager& gdi, u64 hdc, const ThemeObject& t, i32 part, i32 state, i32 font_prop)
        : m_gdi(gdi), m_hdc(static_cast<Handle>(hdc)) {
        const auto* p = find_property(t.variant, part, state, font_prop, nullptr);
        if (p == nullptr || p->type != PropType::Font) return;
        const auto& f = font_record(p->extra);
        GdiManager::FontInfo info;
        info.height = f.height;
        info.width = f.width;
        info.lf_weight = f.weight;
        info.weight = static_cast<u16>(f.weight);
        info.italic = f.italic != 0;
        info.char_set = static_cast<u8>(f.charset);
        info.face_name = std::u16string(f.face);
        m_font = gdi.create_font(info);
        if (m_font != 0) m_old = gdi.select_object(m_hdc, m_font);
    }
    ~ScopedThemeFont() {
        if (m_font == 0) return;
        (void)m_gdi.select_object(m_hdc, m_old);
        (void)m_gdi.delete_object(m_font);
    }
    ScopedThemeFont(const ScopedThemeFont&) = delete;
    ScopedThemeFont& operator=(const ScopedThemeFont&) = delete;

private:
    GdiManager& m_gdi;
    Handle m_hdc;
    Handle m_font = 0;
    Handle m_old = 0;
};

std::optional<std::u16string> read_text(Marshaler& m, GuestAddr text, i32 cch) {
    if (cch < 0) return read_wide(m, text);
    std::u16string s;
    for (i32 i = 0; i < cch; ++i) {
        const auto c = m.read_u16(text + static_cast<GuestAddr>(i) * 2);
        if (c.is_err()) return std::nullopt;
        s.push_back(static_cast<char16_t>(c.value()));
    }
    return s;
}

/// DTTOPTS (uxtheme.h): 64 bytes on x86, 72 on x64.
struct TextOptions {
    u32 flags = 0;
    u32 text_color = 0, border_color = 0, shadow_color = 0;
    i32 shadow_type = 0, shadow_x = 0, shadow_y = 0, border_size = 0, font_prop = 0, color_prop = 0, state = 0;
};

constexpr u32 DTT_TEXTCOLOR = 1, DTT_BORDERCOLOR = 2, DTT_SHADOWCOLOR = 4, DTT_SHADOWTYPE = 8,
              DTT_SHADOWOFFSET = 0x10, DTT_BORDERSIZE = 0x20, DTT_FONTPROP = 0x40, DTT_COLORPROP = 0x80,
              DTT_STATEID = 0x100, DTT_CALCRECT = 0x200, DTT_CALLBACK = 0x1000, DTT_COMPOSITED = 0x2000;
constexpr u32 DT_CALCRECT = 0x400;

/// The text a theme draws, shared by DrawThemeText and DrawThemeTextEx.
u32 draw_text(Marshaler& m, UxThemeModule& mod, const ThemeObject& t, u64 hdc, i32 part, i32 state,
              const std::u16string& text, i32 cch, u32 format, bool grayed, i32 rect[4], const TextOptions* opts,
              bool update_rect) {
    GdiManager& gdi = *mod.services().gdi;
    const Handle dc = static_cast<Handle>(hdc);
    const i32 draw_state = (opts && (opts->flags & DTT_STATEID)) ? opts->state : state;
    const i32 font_prop = (opts && (opts->flags & DTT_FONTPROP)) ? opts->font_prop : tmt::FONT;
    ScopedThemeFont font(gdi, hdc, t, part, draw_state, font_prop);
    const auto palette = mod.palette(t.dark);
    std::optional<u32> colour;
    if (opts && (opts->flags & DTT_TEXTCOLOR)) {
        colour = opts->text_color;
    } else {
        const i32 colour_prop = (opts && (opts->flags & DTT_COLORPROP)) ? opts->color_prop : tmt::TEXTCOLOR;
        if (find_property(t.variant, part, draw_state, colour_prop, nullptr))
            colour = theme_colorref(t, palette, part, draw_state, colour_prop);
    }
    if (grayed) colour = to_colorref(flatten(palette[ThemeColor::DisabledControlText], part_background(t.variant.record().theme_class, palette)));
    const i32 old_mode = gdi.set_bk_mode(dc, 1);  // TRANSPARENT
    const u32 old_colour = gdi.get_text_color(dc);
    std::vector<u8> before;
    const bool composited = opts && (opts->flags & DTT_COMPOSITED);
    if (composited) {
        (void)gdi.read_device_surface(dc, [&](const GdiManager::ReadOnlyDeviceSurface& s) {
            before.assign(s.rgba, s.rgba + static_cast<usize>(s.width) * static_cast<usize>(s.height) * 4u);
        });
    }
    auto pass = [&](i32 dx, i32 dy, u32 c, u32 fmt, i32 r[4]) {
        (void)gdi.set_text_color(dc, c);
        i32 l = r[0] + dx, tp = r[1] + dy, rr = r[2] + dx, b = r[3] + dy;
        const i32 height = gdi.draw_text_ex(dc, text, l, tp, rr, b, fmt, nullptr);
        if (fmt & DT_CALCRECT) { r[0] = l - dx; r[1] = tp - dy; r[2] = rr - dx; r[3] = b - dy; }
        return height;
    };
    const bool calc_only = (format & DT_CALCRECT) != 0;
    if (!calc_only && opts) {
        if ((opts->flags & DTT_SHADOWTYPE) && opts->shadow_type != 0) {
            const u32 sc = (opts->flags & DTT_SHADOWCOLOR) ? opts->shadow_color : 0x808080u;
            const i32 sx = (opts->flags & DTT_SHADOWOFFSET) ? opts->shadow_x : 1;
            const i32 sy = (opts->flags & DTT_SHADOWOFFSET) ? opts->shadow_y : 1;
            i32 r[4] = {rect[0], rect[1], rect[2], rect[3]};
            (void)pass(sx, sy, sc, format, r);
        }
        if ((opts->flags & DTT_BORDERSIZE) && opts->border_size > 0) {
            const u32 bc = (opts->flags & DTT_BORDERCOLOR) ? opts->border_color : 0xFFFFFFu;
            const i32 n = opts->border_size;
            for (i32 dy = -n; dy <= n; dy += n)
                for (i32 dx = -n; dx <= n; dx += n) {
                    if (dx == 0 && dy == 0) continue;
                    i32 r[4] = {rect[0], rect[1], rect[2], rect[3]};
                    (void)pass(dx, dy, bc, format, r);
                }
        }
    }
    i32 r[4] = {rect[0], rect[1], rect[2], rect[3]};
    const i32 height = pass(0, 0, colour.value_or(old_colour), format, r);
    (void)gdi.set_text_color(dc, old_colour);
    (void)gdi.set_bk_mode(dc, old_mode);
    if (composited && !before.empty()) {
        (void)gdi.with_device_surface(dc, [&](GdiManager::DeviceSurface& s) {
            const usize n = std::min(before.size(), static_cast<usize>(s.width) * static_cast<usize>(s.height) * 4u);
            for (usize i = 0; i + 3 < n; i += 4) {
                if (s.rgba[i] != before[i] || s.rgba[i + 1] != before[i + 1] || s.rgba[i + 2] != before[i + 2])
                    s.rgba[i + 3] = 0xFF;
            }
        });
    }
    if (update_rect && calc_only) std::copy(r, r + 4, rect);
    (void)m;
    if (height == 0 && cch != 0 && !text.empty()) return hresult_from_win32(get_last_error());
    if (cch != 0) set_last_error(0);
    return S_OK;
}

/// Read a DTTOPTS; false when its dwSize is not the structure's size.
bool read_text_options(Marshaler& m, GuestAddr p, TextOptions& o) {
    const u32 size = m.cpu_is_64bit() ? 72u : 64u;
    auto u = [&](u32 off) { return m.read_u32(p + off).value_or(0); };
    if (u(0) != size) return false;
    o.flags = u(4);
    o.text_color = u(8);
    o.border_color = u(12);
    o.shadow_color = u(16);
    o.shadow_type = static_cast<i32>(u(20));
    o.shadow_x = static_cast<i32>(u(24));
    o.shadow_y = static_cast<i32>(u(28));
    o.border_size = static_cast<i32>(u(32));
    o.font_prop = static_cast<i32>(u(36));
    o.color_prop = static_cast<i32>(u(40));
    o.state = static_cast<i32>(u(44));
    return true;
}

/// One of the nine sizing-grid segments of @p r containing (x, y).
u16 segment_code(const i32 r[4], const i16 m[4], i32 x, i32 y) {
    constexpr u16 HTCLIENT = 1, HTLEFT = 10, HTRIGHT = 11, HTTOP = 12, HTTOPLEFT = 13, HTTOPRIGHT = 14,
                  HTBOTTOM = 15, HTBOTTOMLEFT = 16, HTBOTTOMRIGHT = 17;
    const bool L = x < r[0] + m[0], R = x >= r[2] - m[1], T = y < r[1] + m[2], B = y >= r[3] - m[3];
    if (T && L) return HTTOPLEFT;
    if (T && R) return HTTOPRIGHT;
    if (B && L) return HTBOTTOMLEFT;
    if (B && R) return HTBOTTOMRIGHT;
    if (T) return HTTOP;
    if (B) return HTBOTTOM;
    if (L) return HTLEFT;
    if (R) return HTRIGHT;
    return HTCLIENT;
}

}  // namespace

void add_draw_exports(DllExports& exports, UxThemeModule& mod_ref) {
    UxThemeModule* mod = &mod_ref;
    auto add = [&](const char* name, u16 ordinal, u16 stdcall, std::function<void(Marshaler&)> fn) {
        exports.entries.push_back(ExportEntry{name, ordinal, std::move(fn), stdcall});
    };

    // HRESULT DrawThemeBackground(HTHEME, HDC, int, int, LPCRECT pRect, LPCRECT pClipRect)
    add("DrawThemeBackground", 55, 24, [mod](Marshaler& m) {
        i32 clip[4];
        const bool has_clip = read_rect(m, m.arg_ptr(5), clip);
        m.set_return(draw_background(m, *mod, m.arg(0), m.arg(1), static_cast<i32>(m.arg(2)),
                                     static_cast<i32>(m.arg(3)), m.arg_ptr(4), has_clip ? clip : nullptr, {}));
    });
    // HRESULT DrawThemeBackgroundEx(HTHEME, HDC, int, int, LPCRECT, const DTBGOPTS*).
    // DTBGOPTS.dwSize must be sizeof(DTBGOPTS) = 24 (else E_FAIL).
    add("DrawThemeBackgroundEx", 47, 24, [mod](Marshaler& m) {
        constexpr u32 DTBG_CLIPRECT = 1, DTBG_OMITBORDER = 4, DTBG_OMITCONTENT = 8, DTBG_MIRRORDC = 0x20,
                      DTBG_NOMIRROR = 0x40;
        const GuestAddr opts = m.arg_ptr(5);
        PartDrawOptions o;
        i32 clip[4];
        bool has_clip = false;
        if (opts != 0) {
            const auto t = mod->theme(m.arg(0));
            if (!t) { m.set_return(E_HANDLE); return; }
            if (m.read_u32(opts).value_or(0) != 24) { m.set_return(E_FAIL); return; }
            const u32 flags = m.read_u32(opts + 4).value_or(0);
            o.omit_border = (flags & DTBG_OMITBORDER) != 0;
            o.omit_content = (flags & DTBG_OMITCONTENT) != 0;
            o.mirror = (flags & DTBG_MIRRORDC) != 0 && (flags & DTBG_NOMIRROR) == 0;
            if (flags & DTBG_CLIPRECT) has_clip = read_rect(m, opts + 8, clip);
        }
        m.set_return(draw_background(m, *mod, m.arg(0), m.arg(1), static_cast<i32>(m.arg(2)),
                                     static_cast<i32>(m.arg(3)), m.arg_ptr(4), has_clip ? clip : nullptr, o));
    });

    // HRESULT DrawThemeEdge(HTHEME, HDC, int, int, LPCRECT pDestRect, UINT uEdge, UINT uFlags, LPRECT pContentRect)
    add("DrawThemeEdge", 56, 32, [mod](Marshaler& m) {
        constexpr u32 BF_LEFT = 1, BF_TOP = 2, BF_RIGHT = 4, BF_BOTTOM = 8, BF_ADJUST = 0x2000;
        const u64 hdc = m.arg(1);
        DrawCheck c = draw_check(*mod, m.arg(0), hdc, m.arg_ptr(4), static_cast<i32>(m.arg(2)),
                                 static_cast<i32>(m.arg(3)));
        if (c.hr != S_OK) { m.set_return(c.hr); return; }
        i32 r[4];
        if (!read_rect(m, m.arg_ptr(4), r)) { m.set_return(E_POINTER); return; }
        const u32 edge = static_cast<u32>(m.arg(5)), flags = static_cast<u32>(m.arg(6));
        if (r[2] > r[0] && r[3] > r[1]) {
            const ThemeObject t = *c.theme;
            const auto palette = mod->palette(t.dark);
            (void)paint_on_dc(*mod->services().gdi, hdc, r, nullptr, [&](i32 w, i32 h, double s, PartCanvas& out) {
                return render_edge(w, h, s, edge, flags, palette, t.dark, out);
            });
        }
        if ((flags & BF_ADJUST) && m.arg_ptr(7) != 0) {
            // One pixel per ring on each side drawn (measured "edge" lines).
            const i32 rings = ((edge & 3) ? 1 : 0) + ((edge & 12) ? 1 : 0);
            i32 a[4] = {r[0], r[1], r[2], r[3]};
            if (flags & BF_LEFT) a[0] += rings;
            if (flags & BF_TOP) a[1] += rings;
            if (flags & BF_RIGHT) a[2] -= rings;
            if (flags & BF_BOTTOM) a[3] -= rings;
            (void)write_rect(m, m.arg_ptr(7), a);
        }
        m.set_return(S_OK);
    });

    // HRESULT DrawThemeIcon(HTHEME, HDC, int, int, LPCRECT, HIMAGELIST, int)
    add("DrawThemeIcon", 57, 28, [mod](Marshaler& m) {
        if (!mod->theme(m.arg(0)) || m.arg(5) == 0) { m.set_return(E_HANDLE); return; }
        unimplemented_once("DrawThemeIcon: image-list icon effects (ICONEFFECT)");
        m.set_return(E_NOTIMPL);
    });

    // HRESULT DrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int cch, DWORD dwTextFlags,
    //                       DWORD dwTextFlags2, LPCRECT)
    add("DrawThemeText", 70, 36, [mod](Marshaler& m) {
        constexpr u32 DTT_GRAYED = 1;
        const u64 hdc = m.arg(1);
        const i32 part = static_cast<i32>(m.arg(2)), state = static_cast<i32>(m.arg(3));
        const GuestAddr text_ptr = m.arg_ptr(4);
        const i32 cch = static_cast<i32>(m.arg(5));
        const auto t = mod->theme(m.arg(0));
        if (!t) { m.set_return(E_HANDLE); return; }
        if (hdc == 0 || mod->services().gdi == nullptr || !mod->services().gdi->has_dc(static_cast<Handle>(hdc))) {
            m.set_return(E_HANDLE);
            return;
        }
        if (text_ptr == 0 && cch == 0) {  // DrawText draws nothing and fails
            m.set_return(hresult_from_win32(get_last_error()));
            return;
        }
        i32 r[4];
        if (text_ptr == 0 || !read_rect(m, m.arg_ptr(8), r)) { m.set_return(E_POINTER); return; }
        if (!find_part_state(t->variant, part, state)) { m.set_return(E_FAIL); return; }
        const auto text = read_text(m, text_ptr, cch);
        if (!text) { m.set_return(E_POINTER); return; }
        const u32 format = static_cast<u32>(m.arg(6)) & ~DT_CALCRECT;  // LPCRECT: nothing to calculate into
        m.set_return(draw_text(m, *mod, *t, hdc, part, state, *text, cch, format,
                               (static_cast<u32>(m.arg(7)) & DTT_GRAYED) != 0, r, nullptr, false));
    });

    // HRESULT DrawThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*)
    add("DrawThemeTextEx", 71, 36, [mod](Marshaler& m) {
        const u64 hdc = m.arg(1);
        const i32 part = static_cast<i32>(m.arg(2)), state = static_cast<i32>(m.arg(3));
        const GuestAddr text_ptr = m.arg_ptr(4), rect_ptr = m.arg_ptr(7), opts_ptr = m.arg_ptr(8);
        const i32 cch = static_cast<i32>(m.arg(5));
        const auto t = mod->theme(m.arg(0));
        if (!t) { m.set_return(E_HANDLE); return; }
        if (hdc == 0 || mod->services().gdi == nullptr || !mod->services().gdi->has_dc(static_cast<Handle>(hdc))) {
            m.set_return(E_HANDLE);
            return;
        }
        i32 r[4];
        if (text_ptr == 0 || !read_rect(m, rect_ptr, r)) { m.set_return(E_POINTER); return; }
        if (!find_part_state(t->variant, part, state)) { m.set_return(E_FAIL); return; }
        TextOptions opts;
        if (opts_ptr != 0 && !read_text_options(m, opts_ptr, opts)) { m.set_return(E_FAIL); return; }
        if (opts.flags & DTT_CALLBACK) {
            unimplemented_once("DrawThemeTextEx: DTT_CALLBACK (guest DrawText callback)");
            m.set_return(E_NOTIMPL);
            return;
        }
        const auto text = read_text(m, text_ptr, cch);
        if (!text) { m.set_return(E_POINTER); return; }
        const u32 format = static_cast<u32>(m.arg(6));
        // The rectangle is calculated only with both DT_CALCRECT and
        // DTT_CALCRECT (measured dttex calcrect / calcrect_dt / dtcalc_noopt).
        const bool calc = (format & DT_CALCRECT) && (opts.flags & DTT_CALCRECT);
        const u32 hr = draw_text(m, *mod, *t, hdc, part, state, *text, cch, calc ? format : (format & ~DT_CALCRECT),
                                 false, r, opts_ptr ? &opts : nullptr, calc);
        if (calc) (void)write_rect(m, rect_ptr, r);
        m.set_return(hr);
    });

    // HRESULT GetThemeTextExtent(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPCRECT pBoundingRect, LPRECT pExtentRect)
    add("GetThemeTextExtent", 181, 36, [mod](Marshaler& m) {
        const u64 hdc = m.arg(1);
        const i32 part = static_cast<i32>(m.arg(2)), state = static_cast<i32>(m.arg(3));
        const auto t = mod->theme(m.arg(0));
        if (!t) { m.set_return(E_HANDLE); return; }
        if (hdc == 0 || mod->services().gdi == nullptr || !mod->services().gdi->has_dc(static_cast<Handle>(hdc))) {
            m.set_return(E_HANDLE);
            return;
        }
        const GuestAddr out = m.arg_ptr(8);
        if (out == 0 || m.arg_ptr(4) == 0) { m.set_return(E_POINTER); return; }
        if (!find_part_state(t->variant, part, state)) { m.set_return(E_FAIL); return; }
        const auto text = read_text(m, m.arg_ptr(4), static_cast<i32>(m.arg(5)));
        if (!text) { m.set_return(E_POINTER); return; }
        i32 r[4] = {0, 0, 0, 0};
        (void)read_rect(m, m.arg_ptr(7), r);
        GdiManager& gdi = *mod->services().gdi;
        ScopedThemeFont font(gdi, hdc, *t, part, state, tmt::FONT);
        (void)gdi.draw_text_ex(static_cast<Handle>(hdc), *text, r[0], r[1], r[2], r[3],
                               static_cast<u32>(m.arg(6)) | DT_CALCRECT, nullptr);
        (void)write_rect(m, out, r);
        set_last_error(0);
        m.set_return(S_OK);
    });

    // HRESULT GetThemeTextMetrics(HTHEME, HDC, int, int, TEXTMETRICW*)
    add("GetThemeTextMetrics", 182, 20, [mod](Marshaler& m) {
        const u64 hdc = m.arg(1);
        const i32 part = static_cast<i32>(m.arg(2)), state = static_cast<i32>(m.arg(3));
        const auto t = mod->theme(m.arg(0));
        if (!t) { m.set_return(E_HANDLE); return; }
        if (hdc == 0 || mod->services().gdi == nullptr || !mod->services().gdi->has_dc(static_cast<Handle>(hdc))) {
            m.set_return(E_HANDLE);
            return;
        }
        const GuestAddr out = m.arg_ptr(4);
        if (out == 0) { m.set_return(E_POINTER); return; }
        if (!find_part_state(t->variant, part, state)) { m.set_return(E_FAIL); return; }
        GdiManager& gdi = *mod->services().gdi;
        ScopedThemeFont font(gdi, hdc, *t, part, state, tmt::FONT);
        const u32 saved = get_last_error();
        const bool ok = gdi.get_text_metrics_w(static_cast<Handle>(hdc), out);
        set_last_error(saved);
        m.set_return(ok ? S_OK : E_FAIL);
    });

    // DrawThemeParentBackground(Ex): the parent paints its background into
    // the child's DC, offset to the child's position: WM_ERASEBKGND, then
    // WM_PRINTCLIENT(PRF_CLIENT), and with DTPB_USECTLCOLORSTATIC
    // WM_CTLCOLORSTATIC (measured order 0014, 0318, 0138). S_OK when the
    // parent's brush painted it, S_FALSE otherwise; a window without a
    // parent is S_OK with ERROR_INVALID_WINDOW_HANDLE.
    auto parent_background = [mod](Marshaler& m, u32 hwnd, u64 hdc, u32 flags, GuestAddr rect_ptr) -> u32 {
        constexpr u32 WM_ERASEBKGND = 0x0014, WM_PRINTCLIENT = 0x0318, WM_CTLCOLORSTATIC = 0x0138,
                      PRF_CLIENT = 4, DTPB_USECTLCOLORSTATIC = 2;
        if (!mod->window_valid(hwnd)) return E_HANDLE;
        const auto& svc = mod->services();
        const u32 parent = svc.parent ? svc.parent(hwnd) : 0;
        if (parent == 0 || !svc.send_message) {
            set_last_error(ERROR_INVALID_WINDOW_HANDLE);
            return S_OK;
        }
        GdiManager* gdi = svc.gdi;
        if (gdi == nullptr || !gdi->has_dc(static_cast<Handle>(hdc))) return E_HANDLE;
        const Handle dc = static_cast<Handle>(hdc);
        i32 ox = 0, oy = 0;
        if (svc.map_point) (void)svc.map_point(hwnd, parent, ox, oy);
        ClipRect saved_clip;
        (void)gdi->get_dc_clip(dc, saved_clip);
        i32 vx = 0, vy = 0;
        (void)gdi->get_viewport_org(dc, vx, vy);
        i32 r[4];
        const bool has_rect = read_rect(m, rect_ptr, r);
        if (has_rect) (void)gdi->intersect_clip_rect(dc, r[0], r[1], r[2], r[3]);
        (void)gdi->set_viewport_org(dc, vx - ox, vy - oy);
        (void)svc.send_message(parent, WM_ERASEBKGND, hdc, 0);
        const u64 printed = svc.send_message(parent, WM_PRINTCLIENT, hdc, PRF_CLIENT);
        u32 hr = printed != 0 ? S_OK : S_FALSE;
        (void)gdi->set_viewport_org(dc, vx, vy);
        if (flags & DTPB_USECTLCOLORSTATIC) {
            const u64 brush = svc.send_message(parent, WM_CTLCOLORSTATIC, hdc, hwnd);
            u32 colour = 0;
            if (brush != 0 && gdi->brush_fill_color(dc, static_cast<Handle>(brush), colour)) {
                i32 area[4] = {0, 0, 0, 0};
                if (has_rect) std::copy(r, r + 4, area);
                else (void)gdi->get_clip_box(dc, area[0], area[1], area[2], area[3]);
                (void)gdi->fill_rect_gdi(dc, area[0], area[1], area[2], area[3], colour);
                hr = S_OK;
            } else if (brush != 0) {
                hr = S_OK;
            }
        }
        gdi->set_dc_clip(dc, saved_clip);
        return hr;
    };
    add("DrawThemeParentBackground", 58, 12, [parent_background](Marshaler& m) {
        m.set_return(parent_background(m, static_cast<u32>(m.arg(0)), m.arg(1), 0, m.arg_ptr(2)));
    });
    add("DrawThemeParentBackgroundEx", 59, 16, [parent_background](Marshaler& m) {
        m.set_return(parent_background(m, static_cast<u32>(m.arg(0)), m.arg(1), static_cast<u32>(m.arg(2)),
                                       m.arg_ptr(3)));
    });

    // HRESULT GetThemeBackgroundRegion(HTHEME, HDC, int, int, LPCRECT, HRGN*):
    // the rectangle for an opaque part, else the artwork's own shape.
    add("GetThemeBackgroundRegion", 156, 24, [mod](Marshaler& m) {
        const auto t = mod->theme(m.arg(0));
        if (!t) { m.set_return(E_HANDLE); return; }
        const GuestAddr out = m.arg_ptr(5);
        i32 r[4];
        if (out == 0 || !read_rect(m, m.arg_ptr(4), r)) { m.set_return(E_POINTER); return; }
        const i32 part = static_cast<i32>(m.arg(2)), state = static_cast<i32>(m.arg(3));
        const auto* ps = find_part_state(t->variant, part, state);
        if (ps == nullptr) { m.set_return(E_FAIL); return; }
        if (ps->flags & kNoRegion) { m.set_return(E_UNEXPECTED); return; }
        GdiManager* gdi = mod->services().gdi;
        if (gdi == nullptr) { m.set_return(E_FAIL); return; }
        const Handle rgn = gdi->create_rect_rgn(r[0], r[1], r[2], r[3]);
        if (rgn == 0) { m.set_return(0x8007000Eu); return; }
        if ((ps->flags & kPartiallyTransparent) && r[2] > r[0] && r[3] > r[1]) {
            PartCanvas canvas;
            if (render_theme_part(*mod, *t, part, state, r[2] - r[0], r[3] - r[1], {}, canvas)) {
                ClipRect shape;
                shape.left = r[0]; shape.top = r[1]; shape.right = r[2]; shape.bottom = r[3];
                shape.valid = true;
                for (i32 y = 0; y < canvas.height; ++y) {
                    i32 x = 0;
                    while (x < canvas.width) {
                        while (x < canvas.width && canvas.bgra[(static_cast<usize>(y) * canvas.width + x) * 4 + 3] < 0x80) ++x;
                        const i32 start = x;
                        while (x < canvas.width && canvas.bgra[(static_cast<usize>(y) * canvas.width + x) * 4 + 3] >= 0x80) ++x;
                        if (x > start) shape.rectangles.push_back({r[0] + start, r[1] + y, r[0] + x, r[1] + y + 1});
                    }
                }
                if (!shape.rectangles.empty()) (void)gdi->set_region(rgn, shape);
            }
        }
        (void)m.write_ptr(out, rgn);
        m.set_return(S_OK);
    });

    // HRESULT HitTestThemeBackground(HTHEME, HDC, int, int, DWORD dwOptions, LPCRECT,
    //     HRGN, POINT ptTest, WORD* pwHitTestCode). POINT is passed by value:
    // two stack slots on x86, one register-sized slot on x64.
    add("HitTestThemeBackground", 186, 40, [mod](Marshaler& m) {
        constexpr u32 HTTB_FIXEDBORDER = 2, HTTB_CAPTION = 4, HTTB_SYSTEMSIZINGMARGINS = 0x200;
        constexpr u16 HTNOWHERE = 0, HTCLIENT = 1, HTCAPTION = 2, HTBORDER = 18;
        const bool x64 = m.cpu_is_64bit();
        i32 px, py;
        GuestAddr out;
        if (x64) {
            const u64 pt = m.arg(7);
            px = static_cast<i32>(pt & 0xFFFFFFFFu);
            py = static_cast<i32>(pt >> 32);
            out = m.arg_ptr(8);
        } else {
            px = static_cast<i32>(m.arg(7));
            py = static_cast<i32>(m.arg(8));
            out = m.arg_ptr(9);
        }
        const auto t = mod->theme(m.arg(0));
        if (!t) { m.set_return(E_HANDLE); return; }
        if (out == 0) { m.set_return(E_POINTER); return; }
        const i32 part = static_cast<i32>(m.arg(2)), state = static_cast<i32>(m.arg(3));
        const auto* ps = find_part_state(t->variant, part, state);
        if (ps == nullptr) { m.set_return(E_FAIL); return; }
        i32 r[4];
        if (!read_rect(m, m.arg_ptr(5), r)) { m.set_return(E_POINTER); return; }
        const u32 options = static_cast<u32>(m.arg(4));
        u16 code = HTNOWHERE;
        if (px >= r[0] && px < r[2] && py >= r[1] && py < r[3]) {
            bool opaque = true;
            if (ps->flags & kPartiallyTransparent) {
                PartCanvas canvas;
                if (render_theme_part(*mod, *t, part, state, r[2] - r[0], r[3] - r[1], {}, canvas)) {
                    const usize i = (static_cast<usize>(py - r[1]) * canvas.width + static_cast<usize>(px - r[0])) * 4 + 3;
                    opaque = i < canvas.bgra.size() && canvas.bgra[i] != 0;
                }
            }
            if (opaque) {
                i16 margins[4] = {ps->sizing_margins[0], ps->sizing_margins[1], ps->sizing_margins[2],
                                  ps->sizing_margins[3]};
                if (!(ps->flags & kHasSizingMargins)) {
                    margins[0] = ps->content[0]; margins[1] = ps->content[2];
                    margins[2] = ps->content[1]; margins[3] = ps->content[3];
                }
                if (options & HTTB_SYSTEMSIZINGMARGINS) {
                    const i32 f = mod->services().system_metric ? mod->services().system_metric(32) : 4;
                    for (auto& v : margins) v = static_cast<i16>(f);
                }
                code = segment_code(r, margins, px, py);
                if (code != HTCLIENT) {
                    if ((options & HTTB_CAPTION) && py < r[1] + margins[2] && code == 12) code = HTCAPTION;
                    else if (options & HTTB_FIXEDBORDER) code = HTBORDER;
                }
            }
        }
        (void)m.write_u16(out, code);
        m.set_return(S_OK);
    });

    // HRESULT GetThemeBitmap(HTHEME, int, int, int iPropId, ULONG dwFlags, HBITMAP*):
    // the part's image strip (TMT_IMAGECOUNT frames, TMT_IMAGELAYOUT), drawn
    // by this renderer. GBF_DIRECT keeps ownership in the theme, GBF_COPY
    // hands the caller a bitmap of its own.
    add("GetThemeBitmap", 157, 24, [mod](Marshaler& m) {
        constexpr u32 GBF_DIRECT = 1, GBF_COPY = 2, GBF_VALIDBITS = 3;
        const auto t = mod->theme(m.arg(0));
        if (!t) { m.set_return(E_HANDLE); return; }
        const GuestAddr out = m.arg_ptr(5);
        if (out == 0) { m.set_return(E_POINTER); return; }
        const i32 part = static_cast<i32>(m.arg(1)), state = static_cast<i32>(m.arg(2));
        const i32 prop = static_cast<i32>(m.arg(3));
        const u32 flags = static_cast<u32>(m.arg(4));
        const auto* ps = find_part_state(t->variant, part, state);
        if (ps == nullptr) { m.set_return(E_FAIL); return; }
        if (flags != GBF_DIRECT && flags != GBF_COPY && flags != GBF_VALIDBITS) { m.set_return(E_INVALIDARG); return; }
        const PropertyRecord* p = find_property(t->variant, part, state, prop, nullptr);
        if (p == nullptr) { m.set_return(E_INVALIDARG); return; }
        if (p->type != PropType::Bitmap) { m.set_return(hresult_from_win32(get_last_error())); return; }
        GdiManager* gdi = mod->services().gdi;
        if (gdi == nullptr) { m.set_return(E_FAIL); return; }
        const auto* count = find_property(t->variant, part, state, tmt::IMAGECOUNT, nullptr);
        const auto* layout = find_property(t->variant, part, state, tmt::IMAGELAYOUT, nullptr);
        const i32 frames = std::max(1, count ? count->value[0] : 1);
        const bool horizontal = layout && layout->value[0] == 1;
        const i32 fw = std::max<i32>(1, ps->true_cx), fh = std::max<i32>(1, ps->true_cy);
        const i32 bw = horizontal ? fw * frames : fw, bh = horizontal ? fh : fh * frames;
        static std::mutex cache_mu;
        static std::unordered_map<u64, Handle> direct;
        const u64 key = (static_cast<u64>(t->variant.index) << 32) | (static_cast<u64>(part & 0xFFFF) << 16) |
                        static_cast<u64>(state & 0xFFFF);
        if (flags != GBF_COPY) {
            std::lock_guard lock(cache_mu);
            if (const auto it = direct.find(key); it != direct.end()) {
                (void)m.write_ptr(out, it->second);
                m.set_return(S_OK);
                return;
            }
        }
        GuestAddr bits = 0;
        const Handle bmp = gdi->create_runtime_dib_section(bw, bh, false, 32, &bits);
        const Handle dc = bmp ? gdi->create_compatible_dc(0) : 0;
        if (bmp == 0 || dc == 0) {
            if (bmp) (void)gdi->delete_object(bmp);
            m.set_return(0x8007000Eu);  // E_OUTOFMEMORY
            return;
        }
        const Handle old = gdi->select_object(dc, bmp);
        const auto palette = mod->palette(t->dark);
        for (i32 i = 0; i < frames; ++i) {
            const i32 rect[4] = {horizontal ? i * fw : 0, horizontal ? 0 : i * fh, horizontal ? (i + 1) * fw : fw,
                                 horizontal ? fh : (i + 1) * fh};
            (void)paint_on_dc(*gdi, dc, rect, nullptr, [&](i32 w, i32 h, double s, PartCanvas& c) {
                return render_part(t->variant.record().theme_class, part, i + 1, w, h, s, palette, t->dark, {}, c);
            });
        }
        (void)gdi->select_object(dc, old);
        (void)gdi->delete_dc(dc);
        if (flags != GBF_COPY) {
            std::lock_guard lock(cache_mu);
            direct[key] = bmp;
        }
        (void)m.write_ptr(out, bmp);
        m.set_return(S_OK);
    });
}

}  // namespace vitreo::winapi::uxtheme
