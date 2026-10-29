/// @file theme_render.cpp
/// @brief macOS artwork for every visual-style part uxtheme draws.
///
/// The shapes follow AppKit's controls (rounded push-button bezels, accent
/// check boxes and radio buttons, capsule sliders and progress tracks,
/// segmented tabs, disclosure chevrons, traffic-light caption buttons, ...)
/// in the colours of a windowing::ThemePalette. The rectangle is always the
/// one the guest passed: Windows' metrics decide the layout, this file only
/// decides the pixels inside it.

#include "winapi/ui/uxtheme/theme_render.hpp"

#include "winapi/graphics/gdi32/gdi32.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>

namespace vitreo::winapi::uxtheme {

namespace {

using windowing::ThemeColor;
using windowing::ThemePalette;

// --- colours ---------------------------------------------------------------

constexpr u32 argb(u32 a, u32 r, u32 g, u32 b) { return (a << 24) | (r << 16) | (g << 8) | b; }

/// @p c with its alpha multiplied by @p f.
u32 fade(u32 c, double f) {
    const double a = std::clamp(((c >> 24) & 0xFF) * f, 0.0, 255.0);
    return (static_cast<u32>(std::lround(a)) << 24) | (c & 0xFFFFFFu);
}

/// @p c with alpha @p a (0..1), whatever its own alpha.
u32 with_alpha(u32 c, double a) {
    return (static_cast<u32>(std::lround(std::clamp(a, 0.0, 1.0) * 255.0)) << 24) | (c & 0xFFFFFFu);
}

/// @p top over @p bottom (straight alpha).
u32 over(u32 top, u32 bottom) {
    const double ta = ((top >> 24) & 0xFF) / 255.0, ba = ((bottom >> 24) & 0xFF) / 255.0;
    const double oa = ta + ba * (1 - ta);
    if (oa <= 0) return 0;
    auto ch = [&](int shift) {
        const double t = ((top >> shift) & 0xFF), b = ((bottom >> shift) & 0xFF);
        return static_cast<u32>(std::lround((t * ta + b * ba * (1 - ta)) / oa)) & 0xFF;
    };
    return (static_cast<u32>(std::lround(oa * 255)) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// --- painter ---------------------------------------------------------------

enum class Dir { Up, Down, Left, Right };

struct Painter {
    CGContextRef cg;
    double w, h, s;  ///< canvas size; device pixels per 96-DPI pixel
    const ThemePalette& p;
    bool dark;
    /// The colour the part sits on. Translucent colours are flattened onto
    /// it, so a part's pixels are opaque except where it has no artwork (and
    /// at anti-aliased edges): repainting a hot or pushed part without first
    /// repainting the background then leaves it unchanged, as Aero's images
    /// do. 0 = keep translucency (overlay parts).
    u32 ground = 0;

    [[nodiscard]] u32 c(ThemeColor k) const { return p[k]; }
    /// A neutral tint for fills on the window: black in light mode, white in dark.
    [[nodiscard]] u32 tint(double a) const { return with_alpha(dark ? 0xFFFFFFu : 0x000000u, a); }

    [[nodiscard]] CGRect all() const { return CGRectMake(0, 0, w, h); }

    [[nodiscard]] u32 flat(u32 v) const {
        return ((ground >> 24) == 0xFF && (v >> 24) != 0 && (v >> 24) != 0xFF) ? over(v, ground) : v;
    }
    void set_fill(u32 v) const {
        v = flat(v);
        CGContextSetRGBFillColor(cg, ((v >> 16) & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0,
                                 (v & 0xFF) / 255.0, ((v >> 24) & 0xFF) / 255.0);
    }
    void set_stroke(u32 v) const {
        v = flat(v);
        CGContextSetRGBStrokeColor(cg, ((v >> 16) & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0,
                                   (v & 0xFF) / 255.0, ((v >> 24) & 0xFF) / 255.0);
    }
    void round_path(CGRect r, double radius) const {
        radius = std::max(0.0, std::min({radius, r.size.width / 2, r.size.height / 2}));
        CGPathRef path = CGPathCreateWithRoundedRect(r, radius, radius, nullptr);
        CGContextAddPath(cg, path);
        CGPathRelease(path);
    }
    void fill(CGRect r, u32 v, double radius = 0) const {
        if (r.size.width <= 0 || r.size.height <= 0 || (v >> 24) == 0) return;
        set_fill(v);
        if (radius <= 0) {
            CGContextFillRect(cg, r);
            return;
        }
        round_path(r, radius);
        CGContextFillPath(cg);
    }
    /// A border of @p width drawn inside @p r.
    void stroke(CGRect r, u32 v, double radius, double width) const {
        if (r.size.width <= width || r.size.height <= width || (v >> 24) == 0) return;
        set_stroke(v);
        CGContextSetLineWidth(cg, width);
        round_path(CGRectInset(r, width / 2, width / 2), std::max(0.0, radius - width / 2));
        CGContextStrokePath(cg);
    }
    void line(double x0, double y0, double x1, double y1, u32 v, double width) const {
        if ((v >> 24) == 0) return;
        set_stroke(v);
        CGContextSetLineWidth(cg, width);
        CGContextSetLineCap(cg, kCGLineCapButt);
        CGContextMoveToPoint(cg, x0, y0);
        CGContextAddLineToPoint(cg, x1, y1);
        CGContextStrokePath(cg);
    }
    /// A horizontal hairline across [x0, x1] centred in device row @p y.
    void hline(double x0, double x1, double y, u32 v) const {
        fill(CGRectMake(x0, std::floor(y), x1 - x0, std::max(1.0, std::round(s))), v);
    }
    void vline(double x, double y0, double y1, u32 v) const {
        fill(CGRectMake(std::floor(x), y0, std::max(1.0, std::round(s)), y1 - y0), v);
    }
    void oval(CGRect r, u32 v) const {
        if ((v >> 24) == 0) return;
        set_fill(v);
        CGContextFillEllipseInRect(cg, r);
    }
    void oval_stroke(CGRect r, u32 v, double width) const {
        if ((v >> 24) == 0) return;
        set_stroke(v);
        CGContextSetLineWidth(cg, width);
        CGContextStrokeEllipseInRect(cg, CGRectInset(r, width / 2, width / 2));
    }
    /// An open chevron ("v" shape) centred on (cx, cy), @p size wide.
    void chevron(double cx, double cy, double size, Dir d, u32 v, double width) const {
        if ((v >> 24) == 0 || size <= 0) return;
        const double a = size / 2, b = size / 4;
        set_stroke(v);
        CGContextSetLineWidth(cg, width);
        CGContextSetLineCap(cg, kCGLineCapRound);
        CGContextSetLineJoin(cg, kCGLineJoinRound);
        switch (d) {
        case Dir::Up:
            CGContextMoveToPoint(cg, cx - a, cy + b);
            CGContextAddLineToPoint(cg, cx, cy - b);
            CGContextAddLineToPoint(cg, cx + a, cy + b);
            break;
        case Dir::Down:
            CGContextMoveToPoint(cg, cx - a, cy - b);
            CGContextAddLineToPoint(cg, cx, cy + b);
            CGContextAddLineToPoint(cg, cx + a, cy - b);
            break;
        case Dir::Left:
            CGContextMoveToPoint(cg, cx + b, cy - a);
            CGContextAddLineToPoint(cg, cx - b, cy);
            CGContextAddLineToPoint(cg, cx + b, cy + a);
            break;
        case Dir::Right:
            CGContextMoveToPoint(cg, cx - b, cy - a);
            CGContextAddLineToPoint(cg, cx + b, cy);
            CGContextAddLineToPoint(cg, cx - b, cy + a);
            break;
        }
        CGContextStrokePath(cg);
    }
    /// A filled triangle pointing @p d, centred on (cx, cy).
    void triangle(double cx, double cy, double size, Dir d, u32 v) const {
        if ((v >> 24) == 0 || size <= 0) return;
        const double a = size / 2, b = size / 4;
        set_fill(v);
        switch (d) {
        case Dir::Up:
            CGContextMoveToPoint(cg, cx - a, cy + b);
            CGContextAddLineToPoint(cg, cx, cy - b);
            CGContextAddLineToPoint(cg, cx + a, cy + b);
            break;
        case Dir::Down:
            CGContextMoveToPoint(cg, cx - a, cy - b);
            CGContextAddLineToPoint(cg, cx, cy + b);
            CGContextAddLineToPoint(cg, cx + a, cy - b);
            break;
        case Dir::Left:
            CGContextMoveToPoint(cg, cx + b, cy - a);
            CGContextAddLineToPoint(cg, cx - b, cy);
            CGContextAddLineToPoint(cg, cx + b, cy + a);
            break;
        case Dir::Right:
            CGContextMoveToPoint(cg, cx - b, cy - a);
            CGContextAddLineToPoint(cg, cx + b, cy);
            CGContextAddLineToPoint(cg, cx - b, cy + a);
            break;
        }
        CGContextClosePath(cg);
        CGContextFillPath(cg);
    }
    /// A check mark inside @p box.
    void check(CGRect box, u32 v, double width) const {
        const double x = box.origin.x, y = box.origin.y, bw = box.size.width, bh = box.size.height;
        set_stroke(v);
        CGContextSetLineWidth(cg, width);
        CGContextSetLineCap(cg, kCGLineCapRound);
        CGContextSetLineJoin(cg, kCGLineJoinRound);
        CGContextMoveToPoint(cg, x + bw * 0.22, y + bh * 0.53);
        CGContextAddLineToPoint(cg, x + bw * 0.42, y + bh * 0.73);
        CGContextAddLineToPoint(cg, x + bw * 0.78, y + bh * 0.28);
        CGContextStrokePath(cg);
    }
    void cross(double cx, double cy, double size, u32 v, double width) const {
        const double a = size / 2;
        line(cx - a, cy - a, cx + a, cy + a, v, width);
        line(cx - a, cy + a, cx + a, cy - a, v, width);
    }
};

// --- shared control pieces -------------------------------------------------

struct Look {
    bool hot = false;
    bool pressed = false;
    bool disabled = false;
    bool focused = false;
    bool selected = false;
};

/// macOS push-button bezel: a rounded control-coloured face over a soft
/// shadow line, darkened while pressed.
void bezel(const Painter& g, CGRect r, const Look& look, double radius) {
    if (r.size.width <= 0 || r.size.height <= 0) return;
    const double a = look.disabled ? 0.5 : 1.0;
    const double px = std::max(1.0, std::round(g.s));
    // Shadow under the face (one pixel lower), then a hairline border.
    CGRect face = CGRectMake(r.origin.x + px * 0.5, r.origin.y + px * 0.5, r.size.width - px,
                             r.size.height - px * 1.5);
    g.fill(CGRectOffset(face, 0, px * 0.5), fade(g.tint(g.dark ? 0.0 : 0.22), a), radius);
    u32 body = g.c(ThemeColor::Control);
    if (g.dark) body = over(with_alpha(0xFFFFFFu, 0.16), g.c(ThemeColor::WindowBackground));
    if (look.pressed) body = over(g.tint(g.dark ? 0.12 : 0.10), body);
    g.fill(face, fade(body, a), radius);
    g.stroke(face, fade(g.tint(g.dark ? 0.10 : 0.08), a), radius, std::max(0.5, g.s * 0.5));
}

/// The accent ring macOS draws round the focused or default control.
void focus_ring(const Painter& g, CGRect r, double radius) {
    g.stroke(r, with_alpha(g.c(ThemeColor::KeyboardFocusIndicator), 0.6), radius,
             std::max(2.0, std::round(2 * g.s)));
}

/// An editable text field: text-background fill with a hairline border.
void field(const Painter& g, CGRect r, const Look& look, bool border, bool content) {
    const double radius = 0;
    if (content) {
        u32 bg = g.c(ThemeColor::TextBackground);
        if (look.disabled) bg = over(g.tint(0.04), g.c(ThemeColor::WindowBackground));
        g.fill(r, bg, radius);
    }
    if (!border) return;
    if (look.focused) {
        focus_ring(g, r, radius);
        return;
    }
    const u32 edge = look.disabled ? g.tint(0.10) : g.tint(look.hot ? 0.30 : 0.22);
    g.stroke(r, edge, radius, std::max(1.0, std::round(g.s)));
}

/// Selection highlight for list, tree and menu items.
void selection(const Painter& g, CGRect r, const Look& look, double radius) {
    if (look.selected) {
        const u32 sel = look.focused || look.hot
                            ? with_alpha(g.c(ThemeColor::Accent), g.dark ? 0.45 : 0.28)
                            : g.c(ThemeColor::UnemphasizedSelectedContentBackground);
        g.fill(r, sel, radius);
    } else if (look.hot) {
        g.fill(r, g.tint(g.dark ? 0.08 : 0.05), radius);
    }
}

double radius_for(const Painter& g, double base) { return base * g.s; }

/// Square box of side @p side centred in the canvas.
CGRect centred_square(const Painter& g, double side) {
    side = std::min({side, g.w, g.h});
    return CGRectMake(std::floor((g.w - side) / 2), std::floor((g.h - side) / 2), side, side);
}

void checkbox(const Painter& g, i32 state) {
    using namespace button;
    const i32 v = (state - 1) % 4;  // NORMAL, HOT, PRESSED, DISABLED
    const i32 kind = (state - 1) / 4;  // UNCHECKED, CHECKED, MIXED, IMPLICIT, EXCLUDED
    if (state < CBS_UNCHECKEDNORMAL || state > CBS_EXCLUDEDDISABLED) return;
    const bool pressed = v == 2, disabled = v == 3;
    const CGRect box = centred_square(g, std::min(g.w, g.h));
    const double radius = std::max(2.0, box.size.width * 0.23);
    const double a = disabled ? 0.45 : 1.0;
    const bool on = kind == 1 || kind == 2;
    if (on) {
        u32 accent = g.c(ThemeColor::Accent);
        if (pressed) accent = over(with_alpha(0x000000u, 0.18), accent);
        g.fill(box, fade(accent, a), radius);
        const u32 mark = fade(g.c(ThemeColor::AlternateSelectedControlText), a);
        if (kind == 1) {
            g.check(box, mark, std::max(1.5, box.size.width * 0.13));
        } else {
            const double y = box.origin.y + box.size.height / 2;
            g.line(box.origin.x + box.size.width * 0.27, y, box.origin.x + box.size.width * 0.73, y, mark,
                   std::max(1.5, box.size.width * 0.13));
        }
        return;
    }
    // NSButton check box: a white (dark: translucent) face with a visible grey rim.
    u32 face = g.dark ? over(with_alpha(0xFFFFFFu, 0.16), g.c(ThemeColor::WindowBackground)) : g.c(ThemeColor::Control);
    if (pressed) face = over(g.tint(0.12), face);
    g.fill(box, fade(face, a), radius);
    g.stroke(box, fade(g.tint(g.dark ? 0.22 : 0.30), a), radius, std::max(1.0, std::round(g.s)));
    if (kind == 3) {  // implicit: a dimmed check
        g.check(box, fade(g.c(ThemeColor::SecondaryLabel), a), std::max(1.5, box.size.width * 0.12));
    } else if (kind == 4) {  // excluded: a cross
        g.cross(box.origin.x + box.size.width / 2, box.origin.y + box.size.height / 2, box.size.width * 0.4,
                fade(g.c(ThemeColor::SecondaryLabel), a), std::max(1.5, box.size.width * 0.12));
    }
}

void radio(const Painter& g, i32 state) {
    using namespace button;
    if (state < RBS_UNCHECKEDNORMAL || state > RBS_CHECKEDDISABLED) return;
    const i32 v = (state - 1) % 4;
    const bool on = state >= RBS_CHECKEDNORMAL, pressed = v == 2, disabled = v == 3;
    const CGRect box = centred_square(g, std::min(g.w, g.h));
    const double a = disabled ? 0.45 : 1.0;
    if (on) {
        u32 accent = g.c(ThemeColor::Accent);
        if (pressed) accent = over(with_alpha(0x000000u, 0.18), accent);
        g.oval(box, fade(accent, a));
        const double dot = box.size.width * 0.38;
        g.oval(CGRectMake(box.origin.x + (box.size.width - dot) / 2, box.origin.y + (box.size.height - dot) / 2, dot,
                          dot),
               fade(g.c(ThemeColor::AlternateSelectedControlText), a));
        return;
    }
    u32 body = g.dark ? over(with_alpha(0xFFFFFFu, 0.16), g.c(ThemeColor::WindowBackground)) : g.c(ThemeColor::Control);
    if (pressed) body = over(g.tint(0.1), body);
    g.oval(box, fade(body, a));
    g.oval_stroke(box, fade(g.tint(g.dark ? 0.22 : 0.30), a), std::max(1.0, std::round(g.s)));
}

// --- per-class artwork ------------------------------------------------------

bool draw_button(const Painter& g, i32 part, i32 state, const PartDrawOptions& o) {
    using namespace button;
    switch (part) {
    case BP_PUSHBUTTON:
    case BP_USERBUTTON:
    case BP_PUSHBUTTONDROPDOWN: {
        Look look;
        look.hot = state == PBS_HOT;
        look.pressed = state == PBS_PRESSED;
        look.disabled = part == BP_PUSHBUTTONDROPDOWN ? state == PBDDS_DISABLED : state == PBS_DISABLED;
        const double radius = radius_for(g, 5);
        const CGRect r = CGRectInset(g.all(), g.s, g.s);
        if (!o.omit_content) bezel(g, r, look, radius);
        if (part == BP_PUSHBUTTON && !o.omit_border &&
            (state == PBS_DEFAULTED || state == PBS_DEFAULTED_ANIMATING)) {
            g.stroke(CGRectInset(r, g.s * 0.5, g.s * 0.5), with_alpha(g.c(ThemeColor::Accent), 0.85), radius,
                     std::max(1.5, 1.5 * g.s));
        }
        if (part == BP_PUSHBUTTONDROPDOWN) {
            g.chevron(g.w / 2, g.h / 2, 7 * g.s, Dir::Down, fade(g.c(ThemeColor::Label), look.disabled ? 0.4 : 0.85),
                      1.5 * g.s);
        }
        return true;
    }
    case BP_RADIOBUTTON:
    case BP_RADIOBUTTON_HCDISABLED:
        radio(g, part == BP_RADIOBUTTON_HCDISABLED ? RBS_UNCHECKEDDISABLED + (state >= RBS_CHECKEDNORMAL ? 4 : 0)
                                                   : state);
        return true;
    case BP_CHECKBOX:
    case BP_CHECKBOX_HCDISABLED:
        checkbox(g, part == BP_CHECKBOX_HCDISABLED ? ((state - 1) / 4) * 4 + CBS_UNCHECKEDDISABLED : state);
        return true;
    case BP_GROUPBOX:
    case BP_GROUPBOX_HCDISABLED: {
        // NSBox: a faint rounded panel with a hairline border.
        const double a = (state == GBS_DISABLED || part == BP_GROUPBOX_HCDISABLED) ? 0.5 : 1.0;
        const double radius = radius_for(g, 5);
        if (!o.omit_content) g.fill(g.all(), fade(g.tint(g.dark ? 0.05 : 0.035), a), radius);
        if (!o.omit_border) g.stroke(g.all(), fade(g.tint(g.dark ? 0.12 : 0.10), a), radius, std::max(1.0, g.s));
        return true;
    }
    case BP_COMMANDLINK: {
        Look look;
        look.hot = state == CMDLS_HOT || state == CMDLS_DEFAULTED_ANIMATING;
        look.pressed = state == CMDLS_PRESSED;
        const double radius = radius_for(g, 6);
        if (look.pressed) g.fill(g.all(), g.tint(0.10), radius);
        else if (look.hot) g.fill(g.all(), g.tint(0.05), radius);
        if (state == CMDLS_DEFAULTED) focus_ring(g, g.all(), radius);
        return true;
    }
    case BP_COMMANDLINKGLYPH: {
        const u32 col = state == CMDLGS_DISABLED ? g.c(ThemeColor::TertiaryLabel) : g.c(ThemeColor::Accent);
        const double size = std::min(g.w, g.h) * 0.5;
        g.chevron(g.w / 2, g.h / 2, size, Dir::Right, col, std::max(1.5, 2 * g.s));
        return true;
    }
    default:
        return false;
    }
}

bool draw_edit(const Painter& g, i32 part, i32 state, const PartDrawOptions& o) {
    using namespace edit;
    Look look;
    switch (part) {
    case EP_EDITTEXT:
        look.hot = state == ETS_HOT;
        look.disabled = state == ETS_DISABLED || state == ETS_READONLY;
        look.focused = state == ETS_FOCUSED;
        field(g, g.all(), look, !o.omit_border, !o.omit_content);
        return true;
    case EP_BACKGROUND:
        look.disabled = state == EBS_DISABLED || state == EBS_READONLY;
        if (!o.omit_content) field(g, g.all(), look, false, true);
        return true;
    case EP_BACKGROUNDWITHBORDER:
        look.hot = state == EBWBS_HOT;
        look.disabled = state == EBWBS_DISABLED;
        look.focused = state == EBWBS_FOCUSED;
        field(g, g.all(), look, !o.omit_border, !o.omit_content);
        return true;
    case EP_EDITBORDER_NOSCROLL:
    case EP_EDITBORDER_HSCROLL:
    case EP_EDITBORDER_VSCROLL:
    case EP_EDITBORDER_HVSCROLL:
        // The four border parts share NORMAL/HOT/FOCUSED/DISABLED = 1..4.
        look.hot = state == EPSN_HOT;
        look.focused = state == EPSN_FOCUSED;
        look.disabled = state == EPSN_DISABLED;
        field(g, g.all(), look, !o.omit_border, false);
        return true;
    case EP_CARET:
        g.fill(g.all(), g.c(ThemeColor::Text));
        return true;
    case EP_PASSWORD: {
        // The password bullet glyph.
        const CGRect dot = centred_square(g, std::min(g.w, g.h) * 0.6);
        g.oval(dot, g.c(ThemeColor::Text));
        return true;
    }
    default:
        return false;
    }
}

/// macOS pop-up button arrows: an accent rounded square with a chevron.
void popup_arrows(const Painter& g, CGRect r, const Look& look) {
    const double side = std::min(r.size.width, r.size.height) - 4 * g.s;
    if (side <= 2) return;
    const CGRect sq = CGRectMake(r.origin.x + (r.size.width - side) / 2, r.origin.y + (r.size.height - side) / 2,
                                 side, side);
    u32 accent = look.disabled ? g.tint(0.12) : g.c(ThemeColor::Accent);
    if (look.pressed) accent = over(with_alpha(0x000000u, 0.18), accent);
    g.fill(sq, accent, radius_for(g, 4));
    const u32 mark = look.disabled ? g.c(ThemeColor::TertiaryLabel) : g.c(ThemeColor::AlternateSelectedControlText);
    g.chevron(sq.origin.x + side / 2, sq.origin.y + side / 2 + side * 0.04, side * 0.45, Dir::Down, mark,
              std::max(1.5, 1.6 * g.s));
}

bool draw_combobox(const Painter& g, i32 part, i32 state, const PartDrawOptions& o) {
    using namespace combobox;
    Look look;
    look.hot = state == CBXS_HOT;
    look.pressed = state == CBXS_PRESSED;
    look.disabled = state == CBXS_DISABLED;
    switch (part) {
    case CP_DROPDOWNBUTTON:
    case CP_DROPDOWNBUTTONRIGHT:
    case CP_DROPDOWNBUTTONLEFT:
        popup_arrows(g, g.all(), look);
        return true;
    case CP_READONLY:
        bezel(g, CGRectInset(g.all(), g.s, g.s), look, radius_for(g, 5));
        return true;
    case CP_BACKGROUND:
        if (!o.omit_content) g.fill(g.all(), g.c(ThemeColor::TextBackground));
        return true;
    case CP_TRANSPARENTBACKGROUND:
        // CBTBS_NORMAL/HOT/DISABLED/FOCUSED: transparent except a hover tint.
        if (state == CBTBS_HOT) g.fill(g.all(), g.tint(0.04), radius_for(g, 4));
        return true;
    case CP_BORDER: {
        Look f;
        f.hot = state == CBB_HOT;
        f.focused = state == CBB_FOCUSED;
        f.disabled = state == CBB_DISABLED;
        field(g, g.all(), f, !o.omit_border, !o.omit_content);
        return true;
    }
    case CP_CUEBANNER:
        return true;  // the cue banner is text only
    case CP_DROPDOWNITEM: {
        Look item;
        item.selected = state == CBDI_HIGHLIGHTED;
        item.focused = item.selected;
        selection(g, CGRectInset(g.all(), 2 * g.s, 0), item, radius_for(g, 4));
        return true;
    }
    default:
        return false;
    }
}

/// The legacy (always shown) macOS scroll bar track.
void scroll_track(const Painter& g, bool vertical, bool pressed) {
    u32 track = g.dark ? argb(0xFF, 0x2A, 0x2A, 0x2A) : argb(0xFF, 0xFA, 0xFA, 0xFA);
    if (pressed) track = over(g.tint(0.06), track);
    g.fill(g.all(), track);
    const u32 edge = g.tint(g.dark ? 0.10 : 0.07);
    if (vertical) g.vline(0, 0, g.h, edge);
    else g.hline(0, g.w, 0, edge);
}

void scroll_thumb(const Painter& g, bool vertical, const Look& look) {
    scroll_track(g, vertical, false);
    const double inset = std::max(2.0, std::round(3 * g.s));
    CGRect knob = CGRectInset(g.all(), vertical ? inset : 0, vertical ? 0 : inset);
    knob = CGRectInset(knob, vertical ? 0 : g.s, vertical ? g.s : 0);
    const double thick = vertical ? knob.size.width : knob.size.height;
    double a = look.pressed ? 0.62 : look.hot ? 0.5 : 0.32;
    if (look.disabled) a = 0.12;
    g.fill(knob, g.tint(a + (g.dark ? 0.1 : 0)), thick / 2);
}

bool draw_scrollbar(const Painter& g, i32 part, i32 state, const PartDrawOptions&) {
    using namespace scrollbar;
    Look look;
    look.hot = state == SCRBS_HOT || state == SCRBS_HOVER;
    look.pressed = state == SCRBS_PRESSED;
    look.disabled = state == SCRBS_DISABLED;
    switch (part) {
    case SBP_ARROWBTN: {
        // macOS scroll bars have no stepper arrows: the button area is track.
        const i32 dir = (state - 1) / 4;  // UP, DOWN, LEFT, RIGHT (+ the HOVER block)
        const bool vertical = state >= ABS_UPHOVER ? (state == ABS_UPHOVER || state == ABS_DOWNHOVER) : dir < 2;
        scroll_track(g, vertical, false);
        return true;
    }
    case SBP_THUMBBTNHORZ:
        scroll_thumb(g, false, look);
        return true;
    case SBP_THUMBBTNVERT:
        scroll_thumb(g, true, look);
        return true;
    case SBP_LOWERTRACKHORZ:
    case SBP_UPPERTRACKHORZ:
        scroll_track(g, false, look.pressed);
        return true;
    case SBP_LOWERTRACKVERT:
    case SBP_UPPERTRACKVERT:
        scroll_track(g, true, look.pressed);
        return true;
    case SBP_GRIPPERHORZ:
    case SBP_GRIPPERVERT:
        return true;  // no gripper on a macOS scroller knob
    case SBP_SIZEBOX:
    case SBP_SIZEBOXBKGND:
        g.fill(g.all(), g.dark ? argb(0xFF, 0x2A, 0x2A, 0x2A) : argb(0xFF, 0xFA, 0xFA, 0xFA));
        return true;
    default:
        return false;
    }
}

bool draw_spin(const Painter& g, i32 part, i32 state, const PartDrawOptions&) {
    using namespace spin;
    Look look;
    look.hot = state == UPS_HOT;
    look.pressed = state == UPS_PRESSED;
    look.disabled = state == UPS_DISABLED;
    // NSStepper: one rounded bezel split in two; each half draws its own end.
    const double radius = radius_for(g, 4);
    const double px = std::max(1.0, std::round(g.s));
    CGRect r = g.all();
    Dir dir = Dir::Up;
    switch (part) {
    case SPNP_UP: r.size.height += radius; dir = Dir::Up; break;
    case SPNP_DOWN: r.origin.y -= radius; r.size.height += radius; dir = Dir::Down; break;
    case SPNP_UPHORZ: r.size.width += radius; dir = Dir::Left; break;  // UPHORZ is the left half
    case SPNP_DOWNHORZ: r.origin.x -= radius; r.size.width += radius; dir = Dir::Right; break;
    default: return false;
    }
    CGContextSaveGState(g.cg);
    CGContextClipToRect(g.cg, g.all());
    bezel(g, CGRectInset(r, px, 0), look, radius);
    CGContextRestoreGState(g.cg);
    const u32 mark = fade(g.c(ThemeColor::Label), look.disabled ? 0.35 : 0.8);
    const double size = std::min(g.w, g.h) * 0.55;
    g.chevron(g.w / 2, g.h / 2, size, dir, mark, std::max(1.2, 1.4 * g.s));
    return true;
}

bool draw_tab(const Painter& g, i32 part, i32 state, const PartDrawOptions& o) {
    using namespace tab;
    const double radius = radius_for(g, 5);
    switch (part) {
    case TABP_PANE:
        if (!o.omit_content) g.fill(g.all(), g.tint(g.dark ? 0.05 : 0.035), radius);
        if (!o.omit_border) g.stroke(g.all(), g.tint(g.dark ? 0.12 : 0.10), radius, std::max(1.0, g.s));
        return true;
    case TABP_BODY:
    case TABP_AEROWIZARDBODY:
        g.fill(g.all(), over(g.tint(g.dark ? 0.05 : 0.035), g.c(ThemeColor::WindowBackground)));
        return true;
    default:
        break;
    }
    if (part < TABP_TABITEM || part > TABP_TOPTABITEMBOTHEDGE) return false;
    // Segmented control: the item is one segment of a grey track; the
    // selected segment is a raised control-coloured face. All item parts
    // share NORMAL/HOT/SELECTED/DISABLED/FOCUSED = 1..5.
    const i32 edge = (part - TABP_TABITEM) % 4;  // 0 middle, 1 left, 2 right, 3 both
    const bool left_end = edge == 1 || edge == 3, right_end = edge == 2 || edge == 3;
    const bool selected = state == TIS_SELECTED || state == TIS_FOCUSED;
    const double a = state == TIS_DISABLED ? 0.5 : 1.0;
    CGRect track = g.all();
    track.size.height = g.h + radius;  // the bottom corners run under the pane
    if (!left_end) { track.origin.x -= radius; track.size.width += radius; }
    if (!right_end) track.size.width += radius;
    CGContextSaveGState(g.cg);
    CGContextClipToRect(g.cg, g.all());
    g.fill(track, fade(g.tint(state == TIS_HOT ? 0.09 : 0.06), a), radius);
    CGContextRestoreGState(g.cg);
    if (!left_end) g.vline(0, g.h * 0.25, g.h * 0.75, g.tint(0.12));
    if (selected) {
        CGRect face = CGRectInset(g.all(), 2 * g.s, 2 * g.s);
        bezel(g, face, Look{false, false, state == TIS_DISABLED}, radius_for(g, 4));
        if (state == TIS_FOCUSED) focus_ring(g, face, radius_for(g, 4));
    }
    return true;
}

/// Capsule band of thickness @p band centred across the canvas.
CGRect band_rect(const Painter& g, bool vertical, double band) {
    if (vertical) {
        band = std::min(band, g.w);
        return CGRectMake(std::round((g.w - band) / 2), 0, band, g.h);
    }
    band = std::min(band, g.h);
    return CGRectMake(0, std::round((g.h - band) / 2), g.w, band);
}

bool draw_progress(const Painter& g, i32 part, i32 state, const PartDrawOptions&) {
    using namespace progress;
    const double band = std::round(6 * g.s);
    const bool vertical = part == PP_BARVERT || part == PP_CHUNKVERT || part == PP_FILLVERT ||
                          part == PP_PULSEOVERLAYVERT || part == PP_MOVEOVERLAYVERT || part == PP_TRANSPARENTBARVERT;
    const CGRect r = band_rect(g, vertical, band);
    const double radius = band / 2;
    switch (part) {
    case PP_BAR:
    case PP_BARVERT:
    case PP_TRANSPARENTBAR:
    case PP_TRANSPARENTBARVERT:
        g.fill(r, g.tint(g.dark ? 0.14 : 0.08), radius);
        return true;
    case PP_CHUNK:
    case PP_CHUNKVERT:
        g.fill(r, g.c(ThemeColor::Accent), radius);
        return true;
    case PP_FILL:
    case PP_FILLVERT: {
        u32 col = g.c(ThemeColor::Accent);
        if (state == PBFS_ERROR) col = g.c(ThemeColor::SystemRed);
        else if (state == PBFS_PAUSED) col = g.c(ThemeColor::SystemYellow);
        else if (state == PBFS_PARTIAL) col = fade(col, 0.6);
        g.fill(r, col, radius);
        return true;
    }
    case PP_PULSEOVERLAY:
    case PP_MOVEOVERLAY:
    case PP_PULSEOVERLAYVERT:
    case PP_MOVEOVERLAYVERT:
        g.fill(r, with_alpha(0xFFFFFFu, 0.35), radius);
        return true;
    default:
        return false;
    }
}

bool draw_trackbar(const Painter& g, i32 part, i32 state, const PartDrawOptions&) {
    using namespace trackbar;
    Look look;
    look.hot = state == TUS_HOT;
    look.pressed = state == TUS_PRESSED;
    look.focused = state == TUS_FOCUSED;
    look.disabled = state == TUS_DISABLED;
    switch (part) {
    case TKP_TRACK:
    case TKP_TRACKVERT: {
        const double band = std::max(3.0, std::round(4 * g.s));
        const CGRect r = band_rect(g, part == TKP_TRACKVERT, band);
        g.fill(r, g.tint(g.dark ? 0.22 : 0.14), band / 2);
        return true;
    }
    case TKP_THUMB: {
        const CGRect knob = centred_square(g, std::min(g.w, g.h));
        bezel(g, knob, look, knob.size.width / 2);
        if (look.focused) focus_ring(g, knob, knob.size.width / 2);
        return true;
    }
    case TKP_THUMBBOTTOM:
    case TKP_THUMBTOP:
    case TKP_THUMBVERT:
    case TKP_THUMBLEFT:
    case TKP_THUMBRIGHT: {
        // The ticked NSSlider knob: a capsule across the track.
        const bool across_h = part == TKP_THUMBBOTTOM || part == TKP_THUMBTOP;
        CGRect knob = g.all();
        if (across_h) knob = CGRectInset(knob, std::max(0.0, (g.w - std::min(g.w, 8 * g.s)) / 2), 0);
        else knob = CGRectInset(knob, 0, std::max(0.0, (g.h - std::min(g.h, 8 * g.s)) / 2));
        const double radius = std::min(knob.size.width, knob.size.height) / 2;
        bezel(g, knob, look, radius);
        if (look.focused) focus_ring(g, knob, radius);
        return true;
    }
    case TKP_TICS:
        g.vline(g.w / 2, 0, g.h, g.c(ThemeColor::TertiaryLabel));
        return true;
    case TKP_TICSVERT:
        g.hline(0, g.w, g.h / 2, g.c(ThemeColor::TertiaryLabel));
        return true;
    default:
        return false;
    }
}

bool draw_toolbar(const Painter& g, i32 part, i32 state, const PartDrawOptions&) {
    using namespace toolbar;
    const double radius = radius_for(g, 5);
    auto background = [&] {
        double a = 0;
        switch (state) {
        case TS_HOT: a = 0.06; break;
        case TS_PRESSED: a = 0.14; break;
        case TS_CHECKED: a = 0.10; break;
        case TS_HOTCHECKED: a = 0.14; break;
        case TS_NEARHOT:
        case TS_OTHERSIDEHOT: a = 0.03; break;
        default: break;
        }
        if (g.dark) a *= 1.4;
        g.fill(g.all(), g.tint(a), radius);
    };
    const u32 glyph = fade(g.c(ThemeColor::Label), state == TS_DISABLED ? 0.3 : 0.75);
    switch (part) {
    case TP_BUTTON:
    case TP_DROPDOWNBUTTON:
    case TP_SPLITBUTTON:
        background();
        return true;
    case TP_SPLITBUTTONDROPDOWN:
        background();
        g.chevron(g.w / 2, g.h / 2, std::min(g.w * 0.5, 7 * g.s), Dir::Down, glyph, 1.4 * g.s);
        return true;
    case TP_DROPDOWNBUTTONGLYPH:
        g.chevron(g.w / 2, g.h / 2, std::min({g.w, g.h, 7 * g.s}), Dir::Down, glyph, 1.4 * g.s);
        return true;
    case TP_SEPARATOR:
        g.vline(g.w / 2, g.h * 0.15, g.h * 0.85, g.c(ThemeColor::Separator));
        return true;
    case TP_SEPARATORVERT:
        g.hline(g.w * 0.15, g.w * 0.85, g.h / 2, g.c(ThemeColor::Separator));
        return true;
    default:
        return false;
    }
}

bool draw_rebar(const Painter& g, i32 part, i32 state, const PartDrawOptions&) {
    using namespace rebar;
    switch (part) {
    case 0:
    case RP_BACKGROUND:
    case RP_BAND:
        g.fill(g.all(), g.c(ThemeColor::WindowBackground));
        return true;
    case RP_GRIPPER:
    case RP_GRIPPERVERT:
        return true;  // macOS toolbars have no grippers
    case RP_CHEVRON:
    case RP_CHEVRONVERT: {
        if (state == CHEVS_HOT || state == CHEVS_PRESSED)
            g.fill(g.all(), g.tint(state == CHEVS_PRESSED ? 0.14 : 0.06), radius_for(g, 4));
        const Dir d = part == RP_CHEVRON ? Dir::Right : Dir::Down;
        const double size = std::min({g.w, g.h}) * 0.4;
        const double step = size * 0.45;
        const u32 col = fade(g.c(ThemeColor::Label), 0.75);
        if (d == Dir::Right) {
            g.chevron(g.w / 2 - step / 2, g.h / 2, size, d, col, 1.4 * g.s);
            g.chevron(g.w / 2 + step / 2, g.h / 2, size, d, col, 1.4 * g.s);
        } else {
            g.chevron(g.w / 2, g.h / 2 - step / 2, size, d, col, 1.4 * g.s);
            g.chevron(g.w / 2, g.h / 2 + step / 2, size, d, col, 1.4 * g.s);
        }
        return true;
    }
    case RP_SPLITTER:
        g.vline(g.w / 2, 0, g.h, g.c(ThemeColor::Separator));
        return true;
    case RP_SPLITTERVERT:
        g.hline(0, g.w, g.h / 2, g.c(ThemeColor::Separator));
        return true;
    default:
        return false;
    }
}

bool draw_header(const Painter& g, i32 part, i32 state, const PartDrawOptions&) {
    using namespace header;
    const u32 sep = g.c(ThemeColor::Separator);
    const u32 glyph = g.c(ThemeColor::SecondaryLabel);
    auto item = [&](bool pressed) {
        u32 bg = g.c(ThemeColor::ControlBackground);
        if (pressed) bg = over(g.tint(0.08), bg);
        g.fill(g.all(), bg);
        g.hline(0, g.w, g.h - std::max(1.0, std::round(g.s)), sep);
        g.vline(g.w - std::max(1.0, std::round(g.s)), g.h * 0.2, g.h * 0.8, sep);
    };
    switch (part) {
    case 0:
        g.fill(g.all(), g.c(ThemeColor::ControlBackground));
        g.hline(0, g.w, g.h - std::max(1.0, std::round(g.s)), sep);
        return true;
    case HP_HEADERITEM:
        item(state == HIS_PRESSED || state == HIS_SORTEDPRESSED || state == HIS_ICONPRESSED ||
             state == HIS_ICONSORTEDPRESSED);
        return true;
    case HP_HEADERITEMLEFT:
    case HP_HEADERITEMRIGHT:
        item(state == HILS_PRESSED);
        return true;
    case HP_HEADERSORTARROW:
        g.chevron(g.w / 2, g.h / 2, std::min({g.w, g.h * 2, 8 * g.s}), state == HSAS_SORTEDUP ? Dir::Up : Dir::Down,
                  glyph, 1.4 * g.s);
        return true;
    case HP_HEADERDROPDOWN:
    case HP_HEADERDROPDOWNFILTER:
        if (state == HDDS_HOT) g.fill(g.all(), g.tint(0.08));
        g.chevron(g.w / 2, g.h / 2, std::min({g.w * 0.6, g.h, 7 * g.s}), Dir::Down, glyph, 1.4 * g.s);
        return true;
    case HP_HEADEROVERFLOW:
        if (state == HOFS_HOT) g.fill(g.all(), g.tint(0.08));
        g.chevron(g.w / 2, g.h / 2, std::min({g.w * 0.6, g.h, 7 * g.s}), Dir::Down, glyph, 1.4 * g.s);
        return true;
    default:
        return false;
    }
}

bool draw_listview(const Painter& g, i32 part, i32 state, const PartDrawOptions&) {
    using namespace listview;
    const double radius = radius_for(g, 4);
    switch (part) {
    case LVP_LISTITEM: {
        Look look;
        look.hot = state == LISS_HOT || state == LISS_HOTSELECTED;
        look.selected = state == LISS_SELECTED || state == LISS_SELECTEDNOTFOCUS || state == LISS_HOTSELECTED;
        look.focused = state == LISS_SELECTED || state == LISS_HOTSELECTED;
        selection(g, g.all(), look, radius);
        return true;
    }
    case LVP_GROUPHEADER: {
        // LVGH_OPEN.. (1-8) then LVGH_CLOSE.. (9-16), each block:
        // plain, hot, selected, selected hot, unfocused, unfocused hot,
        // mixed, mixed hot.
        const i32 v = (state - 1) % 8;
        Look look;
        look.hot = (v % 2) == 1;
        look.selected = v >= 2;
        look.focused = v == 2 || v == 3 || v >= 6;
        selection(g, g.all(), look, radius);
        return true;
    }
    case LVP_GROUPHEADERLINE:
        g.hline(0, g.w, g.h / 2, g.c(ThemeColor::Separator));
        return true;
    case LVP_EXPANDBUTTON:
    case LVP_COLLAPSEBUTTON: {
        if (state == LVEB_HOVER || state == LVEB_PUSHED)
            g.oval(centred_square(g, std::min(g.w, g.h)), g.tint(state == LVEB_PUSHED ? 0.14 : 0.07));
        g.chevron(g.w / 2, g.h / 2, std::min(g.w, g.h) * 0.45, part == LVP_EXPANDBUTTON ? Dir::Down : Dir::Up,
                  g.c(ThemeColor::SecondaryLabel), 1.4 * g.s);
        return true;
    }
    case LVP_LISTGROUP:
    case LVP_LISTDETAIL:
    case LVP_LISTSORTEDDETAIL:
    case LVP_EMPTYTEXT:
    case LVP_COLUMNDETAIL:
        return true;  // the list background shows through
    default:
        return false;
    }
}

bool draw_treeview(const Painter& g, i32 part, i32 state, const PartDrawOptions&) {
    using namespace treeview;
    switch (part) {
    case 0:
        return true;
    case TVP_TREEITEM: {
        Look look;
        look.hot = state == TREIS_HOT || state == TREIS_HOTSELECTED;
        look.selected = state == TREIS_SELECTED || state == TREIS_SELECTEDNOTFOCUS || state == TREIS_HOTSELECTED;
        look.focused = state == TREIS_SELECTED || state == TREIS_HOTSELECTED;
        selection(g, g.all(), look, radius_for(g, 4));
        return true;
    }
    case TVP_GLYPH:
    case TVP_HOTGLYPH: {
        // NSOutlineView disclosure: a chevron pointing right, down when open.
        const bool open = state == GLPS_OPENED;
        const double size = std::min(g.w, g.h) * 0.62;
        const u32 col = part == TVP_HOTGLYPH ? g.c(ThemeColor::Label) : g.c(ThemeColor::SecondaryLabel);
        g.chevron(g.w / 2, g.h / 2, size, open ? Dir::Down : Dir::Right, col, std::max(1.3, 1.5 * g.s));
        return true;
    }
    case TVP_BRANCH:
        return true;  // outline views draw no tree lines
    default:
        return false;
    }
}

u32 menu_background(const Painter& g) {
    return g.dark ? argb(0xFF, 0x2B, 0x2B, 0x2E) : argb(0xFF, 0xF6, 0xF6, 0xF6);
}

void caption_glyph(const Painter& g, i32 kind, u32 col) {
    // kind: 0 close, 1 maximize, 2 minimize, 3 restore
    const double size = std::min(g.w, g.h) * 0.42, cx = g.w / 2, cy = g.h / 2, wdt = std::max(1.0, 1.2 * g.s);
    switch (kind) {
    case 0: g.cross(cx, cy, size, col, wdt); break;
    case 1:
        g.line(cx - size / 2, cy, cx + size / 2, cy, col, wdt);
        g.line(cx, cy - size / 2, cx, cy + size / 2, col, wdt);
        break;
    case 2: g.line(cx - size / 2, cy, cx + size / 2, cy, col, wdt); break;
    default:
        g.triangle(cx - size * 0.15, cy - size * 0.15, size * 0.7, Dir::Up, col);
        g.triangle(cx + size * 0.15, cy + size * 0.15, size * 0.7, Dir::Down, col);
        break;
    }
}

bool draw_menu(const Painter& g, i32 part, i32 state, const PartDrawOptions& o) {
    using namespace menu;
    const u32 label = g.c(ThemeColor::Label);
    switch (part) {
    case MENU_POPUPBACKGROUND:
    case MENU_POPUPGUTTER:
        g.fill(g.all(), menu_background(g));
        return true;
    case MENU_POPUPBORDERS:
        if (!o.omit_content) g.fill(g.all(), menu_background(g));
        if (!o.omit_border) g.stroke(g.all(), g.tint(g.dark ? 0.25 : 0.16), 0, std::max(1.0, std::round(g.s)));
        return true;
    case MENU_POPUPITEM:
    case MENU_POPUPITEMFOCUSABLE: {
        Look look;
        look.selected = state == MPI_HOT || state == MPI_DISABLEDHOT;
        look.focused = state == MPI_HOT;
        if (state == MPI_DISABLEDHOT) {
            g.fill(CGRectInset(g.all(), 5 * g.s, 0), g.tint(0.05), radius_for(g, 4));
        } else {
            selection(g, CGRectInset(g.all(), 5 * g.s, 0), look, radius_for(g, 4));
        }
        return true;
    }
    case MENU_POPUPITEMKBFOCUS:
        focus_ring(g, CGRectInset(g.all(), 5 * g.s, 0), radius_for(g, 4));
        return true;
    case MENU_POPUPSEPARATOR:
        g.hline(5 * g.s, g.w - 5 * g.s, g.h / 2, g.c(ThemeColor::Separator));
        return true;
    case MENU_POPUPCHECK: {
        const bool disabled = state == MC_CHECKMARKDISABLED || state == MC_BULLETDISABLED;
        const u32 col = disabled ? g.c(ThemeColor::TertiaryLabel) : label;
        if (state == MC_BULLETNORMAL || state == MC_BULLETDISABLED) {
            g.oval(centred_square(g, std::min(g.w, g.h) * 0.36), col);
        } else {
            const CGRect box = centred_square(g, std::min(g.w, g.h) * 0.8);
            g.check(box, col, std::max(1.4, 1.6 * g.s));
        }
        return true;
    }
    case MENU_POPUPCHECKBACKGROUND:
        return true;  // macOS draws the mark alone
    case MENU_POPUPSUBMENU:
    case MENU_POPUPSUBMENUHCHOT:
        g.chevron(g.w / 2, g.h / 2, std::min(g.w, g.h) * 0.45, Dir::Right,
                  state == MSM_DISABLED && part == MENU_POPUPSUBMENU ? g.c(ThemeColor::TertiaryLabel) : label,
                  std::max(1.3, 1.5 * g.s));
        return true;
    case MENU_BARBACKGROUND:
        g.fill(g.all(), g.c(ThemeColor::WindowBackground));
        return true;
    case MENU_BARITEM:
        if (state == MBI_HOT || state == MBI_PUSHED || state == MBI_DISABLEDPUSHED)
            g.fill(CGRectInset(g.all(), 0, 2 * g.s), g.tint(state == MBI_HOT ? 0.08 : 0.16), radius_for(g, 4));
        return true;
    case MENU_SYSTEMCLOSE:
    case MENU_SYSTEMCLOSEHCHOT:
        caption_glyph(g, 0, part == MENU_SYSTEMCLOSE && state == MSYSC_DISABLED ? g.c(ThemeColor::TertiaryLabel) : label);
        return true;
    case MENU_SYSTEMMAXIMIZE:
    case MENU_SYSTEMMAXIMIZEHCHOT:
        caption_glyph(g, 1, part == MENU_SYSTEMMAXIMIZE && state == MSYSMX_DISABLED ? g.c(ThemeColor::TertiaryLabel) : label);
        return true;
    case MENU_SYSTEMMINIMIZE:
    case MENU_SYSTEMMINIMIZEHCHOT:
        caption_glyph(g, 2, part == MENU_SYSTEMMINIMIZE && state == MSYSMN_DISABLED ? g.c(ThemeColor::TertiaryLabel) : label);
        return true;
    case MENU_SYSTEMRESTORE:
    case MENU_SYSTEMRESTOREHCHOT:
        caption_glyph(g, 3, part == MENU_SYSTEMRESTORE && state == MSYSR_DISABLED ? g.c(ThemeColor::TertiaryLabel) : label);
        return true;
    case MENU_MENUITEM_TMSCHEMA:
    case MENU_MENUDROPDOWN_TMSCHEMA:
    case MENU_MENUBARITEM_TMSCHEMA:
    case MENU_MENUBARDROPDOWN_TMSCHEMA:
    case MENU_CHEVRON_TMSCHEMA:
        return true;  // the pre-Vista schema parts carry no artwork
    case MENU_SEPARATOR_TMSCHEMA:
        g.hline(0, g.w, g.h / 2, g.c(ThemeColor::Separator));
        return true;
    default:
        return false;
    }
}

bool draw_tooltip(const Painter& g, i32 part, i32 state, const PartDrawOptions& o) {
    using namespace tooltip;
    const u32 bg = g.dark ? argb(0xFF, 0x3A, 0x3A, 0x3C) : argb(0xFF, 0xF8, 0xF8, 0xF8);
    const u32 border = g.tint(g.dark ? 0.3 : 0.18);
    switch (part) {
    case TTP_STANDARD:
    case TTP_STANDARDTITLE:
        if (!o.omit_content) g.fill(g.all(), bg);
        if (!o.omit_border) g.stroke(g.all(), border, 0, std::max(1.0, std::round(g.s)));
        return true;
    case TTP_BALLOON:
    case TTP_BALLOONTITLE:
        if (!o.omit_content) g.fill(g.all(), bg, radius_for(g, 6));
        if (!o.omit_border) g.stroke(g.all(), border, radius_for(g, 6), std::max(1.0, std::round(g.s)));
        return true;
    case TTP_CLOSE:
    case TTP_WRENCH: {
        if (state == TTCS_HOT || state == TTCS_PRESSED)
            g.oval(centred_square(g, std::min(g.w, g.h)), g.tint(state == TTCS_PRESSED ? 0.16 : 0.08));
        if (part == TTP_CLOSE) {
            g.cross(g.w / 2, g.h / 2, std::min(g.w, g.h) * 0.36, g.c(ThemeColor::SecondaryLabel), 1.3 * g.s);
        } else {
            g.oval_stroke(centred_square(g, std::min(g.w, g.h) * 0.45), g.c(ThemeColor::SecondaryLabel), 1.3 * g.s);
        }
        return true;
    }
    case TTP_BALLOONSTEM: {
        const bool up = state <= TTBSS_POINTINGUPRIGHTWALL;
        g.triangle(g.w / 2, g.h / 2, std::min(g.w, g.h * 2), up ? Dir::Up : Dir::Down, bg);
        return true;
    }
    default:
        return false;
    }
}

bool draw_status(const Painter& g, i32 part, i32, const PartDrawOptions&) {
    using namespace status;
    switch (part) {
    case 0:
        g.fill(g.all(), g.c(ThemeColor::WindowBackground));
        g.hline(0, g.w, 0, g.c(ThemeColor::Separator));
        return true;
    case SP_PANE:
        g.vline(g.w - std::max(1.0, std::round(g.s)), g.h * 0.2, g.h * 0.8, g.c(ThemeColor::Separator));
        return true;
    case SP_GRIPPERPANE:
    case SP_GRIPPER:
        return true;  // macOS windows resize from their edges
    default:
        return false;
    }
}

/// A traffic-light caption button.
void traffic_light(const Painter& g, u32 colour, i32 kind, i32 state, bool inactive) {
    // States: NORMAL 1, HOT 2, PUSHED 3, DISABLED 4 for every caption button.
    const CGRect dot = centred_square(g, std::min({g.w, g.h, 12 * g.s}));
    u32 fill = colour;
    if (state == 4 || inactive) fill = g.tint(g.dark ? 0.25 : 0.18);
    if (state == 3) fill = over(with_alpha(0x000000u, 0.2), fill);
    g.oval(dot, fill);
    g.oval_stroke(dot, with_alpha(0x000000u, 0.12), std::max(0.5, 0.5 * g.s));
    if (state == 2 || state == 3) {
        Painter inner = g;
        inner.w = dot.size.width;
        inner.h = dot.size.height;
        CGContextSaveGState(g.cg);
        CGContextTranslateCTM(g.cg, dot.origin.x, dot.origin.y);
        caption_glyph(inner, kind, with_alpha(0x000000u, 0.55));
        CGContextRestoreGState(g.cg);
    }
}

bool draw_window(const Painter& g, i32 part, i32 state, const PartDrawOptions&) {
    using namespace window;
    const bool inactive_frame = state == FS_INACTIVE;
    const u32 bar = g.dark ? (inactive_frame ? argb(0xFF, 0x2D, 0x2D, 0x2D) : argb(0xFF, 0x38, 0x38, 0x38))
                           : (inactive_frame ? argb(0xFF, 0xF6, 0xF6, 0xF6) : argb(0xFF, 0xE8, 0xE8, 0xE8));
    switch (part) {
    case WP_CAPTION:
    case WP_SMALLCAPTION:
    case WP_MINCAPTION:
    case WP_SMALLMINCAPTION:
    case WP_MAXCAPTION:
    case WP_SMALLMAXCAPTION:
    case WP_CAPTIONSIZINGTEMPLATE:
    case WP_SMALLCAPTIONSIZINGTEMPLATE:
        g.fill(g.all(), bar);
        g.hline(0, g.w, g.h - std::max(1.0, std::round(g.s)), g.c(ThemeColor::Separator));
        return true;
    case WP_FRAMELEFT:
    case WP_FRAMERIGHT:
    case WP_FRAMEBOTTOM:
    case WP_SMALLFRAMELEFT:
    case WP_SMALLFRAMERIGHT:
    case WP_SMALLFRAMEBOTTOM:
    case WP_FRAMELEFTSIZINGTEMPLATE:
    case WP_SMALLFRAMELEFTSIZINGTEMPLATE:
    case WP_FRAMERIGHTSIZINGTEMPLATE:
    case WP_SMALLFRAMERIGHTSIZINGTEMPLATE:
    case WP_FRAMEBOTTOMSIZINGTEMPLATE:
    case WP_SMALLFRAMEBOTTOMSIZINGTEMPLATE:
    case WP_FRAME:
        g.fill(g.all(), bar);
        return true;
    case WP_BORDER:
        g.stroke(g.all(), g.tint(g.dark ? 0.3 : 0.2), 0, std::max(1.0, std::round(g.s)));
        return true;
    case WP_DIALOG:
        g.fill(g.all(), g.c(ThemeColor::WindowBackground));
        return true;
    case WP_SYSBUTTON:
    case WP_MDISYSBUTTON:
        return true;  // macOS title bars have no system-menu icon button
    case WP_CLOSEBUTTON:
    case WP_SMALLCLOSEBUTTON:
    case WP_MDICLOSEBUTTON:
        traffic_light(g, argb(0xFF, 0xFF, 0x5F, 0x57), 0, state, false);
        return true;
    case WP_MINBUTTON:
    case WP_MDIMINBUTTON:
        traffic_light(g, argb(0xFF, 0xFE, 0xBC, 0x2E), 2, state, false);
        return true;
    case WP_MAXBUTTON:
        traffic_light(g, argb(0xFF, 0x28, 0xC8, 0x40), 1, state, false);
        return true;
    case WP_RESTOREBUTTON:
    case WP_MDIRESTOREBUTTON:
        traffic_light(g, argb(0xFF, 0x28, 0xC8, 0x40), 3, state, false);
        return true;
    case WP_HELPBUTTON:
    case WP_MDIHELPBUTTON: {
        const CGRect dot = centred_square(g, std::min({g.w, g.h, 14 * g.s}));
        bezel(g, dot, Look{false, state == HBS_PUSHED, state == HBS_DISABLED}, dot.size.width / 2);
        g.oval(centred_square(g, std::max(2.0, 2 * g.s)), g.c(ThemeColor::SecondaryLabel));
        return true;
    }
    case WP_HORZSCROLL:
        scroll_track(g, false, state == HSS_PUSHED);
        return true;
    case WP_VERTSCROLL:
        scroll_track(g, true, state == VSS_PUSHED);
        return true;
    case WP_HORZTHUMB:
        scroll_thumb(g, false, Look{state == HTS_HOT, state == HTS_PUSHED, state == HTS_DISABLED});
        return true;
    case WP_VERTTHUMB:
        scroll_thumb(g, true, Look{state == VTS_HOT, state == VTS_PUSHED, state == VTS_DISABLED});
        return true;
    default:
        return false;
    }
}

/// The opaque colour a part's artwork sits on (see Painter::ground).
u32 part_ground(const Painter& g, ThemeClass cls, i32 part) {
    auto solid = [](u32 c, u32 under) { return over(c, under | 0xFF000000u); };
    const u32 window = g.c(ThemeColor::WindowBackground) | 0xFF000000u;
    const u32 field = solid(g.c(ThemeColor::TextBackground), window);
    switch (cls) {
    case ThemeClass::Edit: case ThemeClass::Listview: case ThemeClass::Treeview: case ThemeClass::Header:
        return solid(g.c(ThemeColor::ControlBackground), window);
    case ThemeClass::Combobox:
        return (part == combobox::CP_READONLY || part == combobox::CP_TRANSPARENTBACKGROUND) ? window : field;
    case ThemeClass::Menu:
        return (part == menu::MENU_BARBACKGROUND || part == menu::MENU_BARITEM) ? window : menu_background(g);
    case ThemeClass::Tooltip:
        return g.dark ? argb(0xFF, 0x3A, 0x3A, 0x3C) : argb(0xFF, 0xF8, 0xF8, 0xF8);
    case ThemeClass::Progress:
        // The pulse/move overlays are translucent highlights over the fill.
        return (part >= progress::PP_PULSEOVERLAY && part <= progress::PP_MOVEOVERLAYVERT) ? 0u : window;
    default:
        return window;
    }
}

}  // namespace

windowing::ThemePalette default_palette(windowing::ThemeAppearance appearance) {
    // AppKit's documented system colours (macOS 14), sRGB.
    ThemePalette p;
    auto set = [&](ThemeColor k, u32 v) { p.argb[static_cast<usize>(k)] = v; };
    if (appearance == windowing::ThemeAppearance::Dark) {
        set(ThemeColor::WindowBackground, argb(0xFF, 0x32, 0x32, 0x32));
        set(ThemeColor::UnderPageBackground, argb(0xFF, 0x28, 0x28, 0x28));
        set(ThemeColor::ControlBackground, argb(0xFF, 0x1E, 0x1E, 0x1E));
        set(ThemeColor::Control, argb(0x3F, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::ControlText, argb(0xD8, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::DisabledControlText, argb(0x3F, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::Label, argb(0xD8, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::SecondaryLabel, argb(0x8C, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::TertiaryLabel, argb(0x3F, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::QuaternaryLabel, argb(0x19, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::Separator, argb(0x19, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::Grid, argb(0xFF, 0x1A, 0x1A, 0x1A));
        set(ThemeColor::TextBackground, argb(0xFF, 0x1E, 0x1E, 0x1E));
        set(ThemeColor::Text, argb(0xFF, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::SelectedTextBackground, argb(0xFF, 0x3F, 0x63, 0x8B));
        set(ThemeColor::Accent, argb(0xFF, 0x0A, 0x84, 0xFF));
        set(ThemeColor::SelectedContentBackground, argb(0xFF, 0x00, 0x58, 0xD0));
        set(ThemeColor::UnemphasizedSelectedContentBackground, argb(0xFF, 0x46, 0x46, 0x46));
        set(ThemeColor::AlternateSelectedControlText, argb(0xFF, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::KeyboardFocusIndicator, argb(0x7F, 0x1A, 0xA9, 0xFF));
        set(ThemeColor::Link, argb(0xFF, 0x41, 0x9C, 0xFF));
        set(ThemeColor::HeaderText, argb(0xFF, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::ShadowColor, argb(0xFF, 0x00, 0x00, 0x00));
        set(ThemeColor::SystemRed, argb(0xFF, 0xFF, 0x45, 0x3A));
        set(ThemeColor::SystemYellow, argb(0xFF, 0xFF, 0xD6, 0x0A));
        set(ThemeColor::SystemGreen, argb(0xFF, 0x32, 0xD7, 0x4B));
        set(ThemeColor::SystemGray, argb(0xFF, 0x98, 0x98, 0x9D));
    } else {
        set(ThemeColor::WindowBackground, argb(0xFF, 0xEC, 0xEC, 0xEC));
        set(ThemeColor::UnderPageBackground, argb(0xFF, 0x96, 0x96, 0x96));
        set(ThemeColor::ControlBackground, argb(0xFF, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::Control, argb(0xFF, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::ControlText, argb(0xD8, 0x00, 0x00, 0x00));
        set(ThemeColor::DisabledControlText, argb(0x3F, 0x00, 0x00, 0x00));
        set(ThemeColor::Label, argb(0xD8, 0x00, 0x00, 0x00));
        set(ThemeColor::SecondaryLabel, argb(0x7F, 0x00, 0x00, 0x00));
        set(ThemeColor::TertiaryLabel, argb(0x42, 0x00, 0x00, 0x00));
        set(ThemeColor::QuaternaryLabel, argb(0x19, 0x00, 0x00, 0x00));
        set(ThemeColor::Separator, argb(0x19, 0x00, 0x00, 0x00));
        set(ThemeColor::Grid, argb(0xFF, 0xE6, 0xE6, 0xE6));
        set(ThemeColor::TextBackground, argb(0xFF, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::Text, argb(0xFF, 0x00, 0x00, 0x00));
        set(ThemeColor::SelectedTextBackground, argb(0xFF, 0xB3, 0xD7, 0xFF));
        set(ThemeColor::Accent, argb(0xFF, 0x00, 0x7A, 0xFF));
        set(ThemeColor::SelectedContentBackground, argb(0xFF, 0x00, 0x63, 0xE1));
        set(ThemeColor::UnemphasizedSelectedContentBackground, argb(0xFF, 0xDC, 0xDC, 0xDC));
        set(ThemeColor::AlternateSelectedControlText, argb(0xFF, 0xFF, 0xFF, 0xFF));
        set(ThemeColor::KeyboardFocusIndicator, argb(0x7F, 0x00, 0x67, 0xF4));
        set(ThemeColor::Link, argb(0xFF, 0x00, 0x68, 0xDA));
        set(ThemeColor::HeaderText, argb(0xD8, 0x00, 0x00, 0x00));
        set(ThemeColor::ShadowColor, argb(0xFF, 0x00, 0x00, 0x00));
        set(ThemeColor::SystemRed, argb(0xFF, 0xFF, 0x3B, 0x30));
        set(ThemeColor::SystemYellow, argb(0xFF, 0xFF, 0xCC, 0x00));
        set(ThemeColor::SystemGreen, argb(0xFF, 0x28, 0xCD, 0x41));
        set(ThemeColor::SystemGray, argb(0xFF, 0x8E, 0x8E, 0x93));
    }
    return p;
}

bool render_part(ThemeClass cls, i32 part, i32 state, i32 width, i32 height, double scale,
                 const windowing::ThemePalette& palette, bool dark, const PartDrawOptions& options,
                 PartCanvas& out) {
    out.width = std::max(0, width);
    out.height = std::max(0, height);
    out.bgra.assign(static_cast<usize>(out.width) * static_cast<usize>(out.height) * 4u, 0);
    if (out.width == 0 || out.height == 0) return false;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(out.bgra.data(), static_cast<size_t>(out.width),
                                            static_cast<size_t>(out.height), 8,
                                            static_cast<size_t>(out.width) * 4u, space,
                                            static_cast<u32>(kCGImageAlphaPremultipliedFirst) |
                                                static_cast<u32>(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(space);
    if (cg == nullptr) return false;
    CGContextTranslateCTM(cg, 0, out.height);
    CGContextScaleCTM(cg, 1, -1);
    if (options.mirror) {
        CGContextTranslateCTM(cg, out.width, 0);
        CGContextScaleCTM(cg, -1, 1);
    }
    CGContextSetShouldAntialias(cg, true);
    Painter g{cg, static_cast<double>(out.width), static_cast<double>(out.height), std::max(0.25, scale),
              palette, dark};
    g.ground = part_ground(g, cls, part);
    if (g.ground != 0 && options.opaque) g.fill(g.all(), g.ground);
    bool drawn = false;
    switch (cls) {
    case ThemeClass::Button: drawn = draw_button(g, part, state, options); break;
    case ThemeClass::Edit: drawn = draw_edit(g, part, state, options); break;
    case ThemeClass::Combobox: drawn = draw_combobox(g, part, state, options); break;
    case ThemeClass::Scrollbar: drawn = draw_scrollbar(g, part, state, options); break;
    case ThemeClass::Spin: drawn = draw_spin(g, part, state, options); break;
    case ThemeClass::Tab: drawn = draw_tab(g, part, state, options); break;
    case ThemeClass::Progress: drawn = draw_progress(g, part, state, options); break;
    case ThemeClass::Trackbar: drawn = draw_trackbar(g, part, state, options); break;
    case ThemeClass::Toolbar: drawn = draw_toolbar(g, part, state, options); break;
    case ThemeClass::Rebar: drawn = draw_rebar(g, part, state, options); break;
    case ThemeClass::Header: drawn = draw_header(g, part, state, options); break;
    case ThemeClass::Listview: drawn = draw_listview(g, part, state, options); break;
    case ThemeClass::Treeview: drawn = draw_treeview(g, part, state, options); break;
    case ThemeClass::Menu: drawn = draw_menu(g, part, state, options); break;
    case ThemeClass::Tooltip: drawn = draw_tooltip(g, part, state, options); break;
    case ThemeClass::Status: drawn = draw_status(g, part, state, options); break;
    case ThemeClass::Window: drawn = draw_window(g, part, state, options); break;
    case ThemeClass::Count: break;
    }
    CGContextRelease(cg);
    return drawn;
}

namespace {

/// A canvas the CG painter draws into; false when CG has no context.
template <typename Draw>
bool with_canvas(i32 width, i32 height, double scale, const ThemePalette& palette, bool dark, PartCanvas& out,
                 Draw&& draw) {
    out.width = std::max(0, width);
    out.height = std::max(0, height);
    out.bgra.assign(static_cast<usize>(out.width) * static_cast<usize>(out.height) * 4u, 0);
    if (out.width == 0 || out.height == 0) return false;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(out.bgra.data(), static_cast<size_t>(out.width),
                                            static_cast<size_t>(out.height), 8,
                                            static_cast<size_t>(out.width) * 4u, space,
                                            static_cast<u32>(kCGImageAlphaPremultipliedFirst) |
                                                static_cast<u32>(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(space);
    if (cg == nullptr) return false;
    CGContextTranslateCTM(cg, 0, out.height);
    CGContextScaleCTM(cg, 1, -1);
    const Painter g{cg, static_cast<double>(out.width), static_cast<double>(out.height), std::max(0.25, scale),
                    palette, dark};
    const bool drawn = draw(g);
    CGContextRelease(cg);
    return drawn;
}

}  // namespace

bool render_edge(i32 width, i32 height, double scale, u32 edge, u32 flags, const windowing::ThemePalette& palette,
                 bool dark, PartCanvas& out) {
    constexpr u32 BDR_RAISEDOUTER = 1, BDR_SUNKENOUTER = 2, BDR_RAISEDINNER = 4, BDR_SUNKENINNER = 8;
    constexpr u32 BF_LEFT = 1, BF_TOP = 2, BF_RIGHT = 4, BF_BOTTOM = 8, BF_DIAGONAL = 0x10, BF_MIDDLE = 0x800,
                  BF_FLAT = 0x4000, BF_MONO = 0x8000;
    return with_canvas(width, height, scale, palette, dark, out, [&](const Painter& g) {
        const double px = std::max(1.0, std::round(g.s));
        if (flags & BF_DIAGONAL) {
            // A diagonal edge is one hairline across the rectangle.
            const bool up = (flags & (BF_LEFT | BF_BOTTOM)) == (BF_LEFT | BF_BOTTOM) ||
                            (flags & (BF_TOP | BF_RIGHT)) == (BF_TOP | BF_RIGHT);
            if (up) g.line(0, g.h, g.w, 0, g.tint(0.25), px);
            else g.line(0, 0, g.w, g.h, g.tint(0.25), px);
            return true;
        }
        CGRect r = g.all();
        auto ring = [&](u32 top_left, u32 bottom_right) {
            if (flags & BF_TOP) g.fill(CGRectMake(r.origin.x, r.origin.y, r.size.width, px), top_left);
            if (flags & BF_LEFT) g.fill(CGRectMake(r.origin.x, r.origin.y, px, r.size.height), top_left);
            if (flags & BF_BOTTOM)
                g.fill(CGRectMake(r.origin.x, r.origin.y + r.size.height - px, r.size.width, px), bottom_right);
            if (flags & BF_RIGHT)
                g.fill(CGRectMake(r.origin.x + r.size.width - px, r.origin.y, px, r.size.height), bottom_right);
            if (flags & BF_LEFT) { r.origin.x += px; r.size.width -= px; }
            if (flags & BF_TOP) { r.origin.y += px; r.size.height -= px; }
            if (flags & BF_RIGHT) r.size.width -= px;
            if (flags & BF_BOTTOM) r.size.height -= px;
        };
        const u32 outer = edge & (BDR_RAISEDOUTER | BDR_SUNKENOUTER), inner = edge & (BDR_RAISEDINNER | BDR_SUNKENINNER);
        const u32 light = with_alpha(0xFFFFFFu, g.dark ? 0.08 : 0.7), mid = g.tint(g.dark ? 0.3 : 0.18),
                  faint = g.tint(g.dark ? 0.16 : 0.08);
        if (flags & BF_MONO) {
            if (outer) ring(g.c(ThemeColor::Label), g.c(ThemeColor::Label));
            if (inner) ring(g.c(ThemeColor::TextBackground), g.c(ThemeColor::TextBackground));
        } else if (flags & BF_FLAT) {
            if (outer) ring(mid, mid);
            if (inner) ring(faint, faint);
        } else {
            if (outer & BDR_RAISEDOUTER) ring(faint, mid);
            else if (outer) ring(mid, faint);
            if (inner & BDR_RAISEDINNER) ring(light, faint);
            else if (inner) ring(faint, light);
        }
        if (flags & BF_MIDDLE) {
            const u32 face = g.dark ? over(with_alpha(0xFFFFFFu, 0.16), g.c(ThemeColor::WindowBackground))
                                    : g.c(ThemeColor::WindowBackground);
            g.fill(r, face);
        }
        return true;
    });
}

bool paint_on_dc(GdiManager& gdi, u64 hdc, const i32 rect[4], const i32* clip_rect,
                 const std::function<bool(i32, i32, double, PartCanvas&)>& render) {
    return gdi.with_device_surface(static_cast<Handle>(hdc), [&](GdiManager::DeviceSurface& s) {
        auto dev_x = [&](double x, double y) { return s.xform[0] * x + s.xform[2] * y + s.xform[4]; };
        auto dev_y = [&](double x, double y) { return s.xform[1] * x + s.xform[3] * y + s.xform[5]; };
        const double x0 = dev_x(rect[0], rect[1]), x1 = dev_x(rect[2], rect[3]);
        const double y0 = dev_y(rect[0], rect[1]), y1 = dev_y(rect[2], rect[3]);
        const i32 ox = static_cast<i32>(std::lround(std::min(x0, x1)));
        const i32 oy = static_cast<i32>(std::lround(std::min(y0, y1)));
        const i32 w = static_cast<i32>(std::lround(std::fabs(x1 - x0)));
        const i32 h = static_cast<i32>(std::lround(std::fabs(y1 - y0)));
        const double scale = std::max(std::fabs(s.xform[0]) + std::fabs(s.xform[2]),
                                      std::fabs(s.xform[1]) + std::fabs(s.xform[3]));
        PartCanvas canvas;
        if (w <= 0 || h <= 0 || !render(w, h, scale > 0 ? scale : 1.0, canvas)) return;
        if (canvas.width != w || canvas.height != h) return;
        i32 cl = 0, ct = 0, cr = s.width, cb = s.height;
        if (clip_rect != nullptr) {
            const double c0 = dev_x(clip_rect[0], clip_rect[1]), c1 = dev_x(clip_rect[2], clip_rect[3]);
            const double d0 = dev_y(clip_rect[0], clip_rect[1]), d1 = dev_y(clip_rect[2], clip_rect[3]);
            cl = std::max(cl, static_cast<i32>(std::lround(std::min(c0, c1))));
            cr = std::min(cr, static_cast<i32>(std::lround(std::max(c0, c1))));
            ct = std::max(ct, static_cast<i32>(std::lround(std::min(d0, d1))));
            cb = std::min(cb, static_cast<i32>(std::lround(std::max(d0, d1))));
        }
        const bool window = s.alpha == 0xFF;
        for (i32 y = 0; y < canvas.height; ++y) {
            const i32 dy = oy + y;
            if (dy < ct || dy >= cb) continue;
            const u8* src = &canvas.bgra[static_cast<usize>(y) * static_cast<usize>(canvas.width) * 4u];
            u8* row = s.rgba + static_cast<usize>(dy) * static_cast<usize>(s.width) * 4u;
            for (i32 x = 0; x < canvas.width; ++x, src += 4) {
                const u32 sa = src[3];
                if (sa == 0) continue;
                const i32 dx = ox + x;
                if (dx < cl || dx >= cr) continue;
                if (s.clip != nullptr && s.clip->valid && !s.clip->contains(dx, dy)) continue;
                u8* d = row + static_cast<usize>(dx) * 4u;
                const u32 inv = 255u - sa;
                u32 r = src[2] + (d[0] * inv + 127u) / 255u;
                u32 gg = src[1] + (d[1] * inv + 127u) / 255u;
                u32 b = src[0] + (d[2] * inv + 127u) / 255u;
                if (s.mono) {
                    const u32 lum = (r * 299u + gg * 587u + b * 114u) / 1000u;
                    r = gg = b = lum >= 128u ? 255u : 0u;
                }
                d[0] = static_cast<u8>(std::min(255u, r));
                d[1] = static_cast<u8>(std::min(255u, gg));
                d[2] = static_cast<u8>(std::min(255u, b));
                d[3] = window ? 0xFF : static_cast<u8>(std::min(255u, sa + (d[3] * inv + 127u) / 255u));
            }
        }
    });
}

}  // namespace vitreo::winapi::uxtheme
