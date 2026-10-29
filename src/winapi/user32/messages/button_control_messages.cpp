// SPDX-License-Identifier: LGPL-2.1-only

/// @file button_control_messages.cpp
/// @brief Button class messages that carry guest pointers: BM_SETIMAGE /
/// BM_GETIMAGE and the comctl32 v6 BCM_* family.
///
/// Contract: Microsoft Learn (BCM_GETIDEALSIZE, BCM_SETIMAGELIST,
/// BCM_GETIMAGELIST, BCM_SETTEXTMARGIN, BCM_GETTEXTMARGIN,
/// BCM_SETDROPDOWNSTATE, BCM_SETSPLITINFO, BCM_GETSPLITINFO, BCM_SETNOTE,
/// BCM_GETNOTE, BCM_GETNOTELENGTH, BCM_SETSHIELD, BM_SETIMAGE, BM_GETIMAGE),
/// CommCtrl.h for the values and structure layouts, and
/// tools/oracle_probe/button_bcm_probe.c (button_bcm_probe{64,32}.expected,
/// button_bcm_probe64.nov6.expected) for everything Learn leaves open: the
/// return values, defaults, and how the ideal size is built from the text.
/// Gaps: .docs/contracts/comctl32.md "Button BCM_* messages".

#include "message_send.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace vitreo::winapi {
namespace {

constexpr u32 BM_GETIMAGE = 0x00F6;
constexpr u32 BM_SETIMAGE = 0x00F7;
constexpr u32 BCM_GETIDEALSIZE = 0x1601;
constexpr u32 BCM_SETIMAGELIST = 0x1602;
constexpr u32 BCM_GETIMAGELIST = 0x1603;
constexpr u32 BCM_SETTEXTMARGIN = 0x1604;
constexpr u32 BCM_GETTEXTMARGIN = 0x1605;
constexpr u32 BCM_SETDROPDOWNSTATE = 0x1606;
constexpr u32 BCM_SETSPLITINFO = 0x1607;
constexpr u32 BCM_GETSPLITINFO = 0x1608;
constexpr u32 BCM_SETNOTE = 0x1609;
constexpr u32 BCM_GETNOTE = 0x160A;
constexpr u32 BCM_GETNOTELENGTH = 0x160B;
constexpr u32 BCM_SETSHIELD = 0x160C;

constexpr u32 IMAGE_BITMAP = 0;
constexpr u32 IMAGE_ICON = 1;

constexpr u32 BCSIF_GLYPH = 0x1, BCSIF_IMAGE = 0x2, BCSIF_STYLE = 0x4, BCSIF_SIZE = 0x8;

// Button types (BS_TYPEMASK) and modifiers (WinUser.h / CommCtrl.h).
constexpr u32 BS_TYPEMASK = 0xF;
constexpr u32 BS_PUSHBUTTON = 0x0, BS_DEFPUSHBUTTON = 0x1, BS_CHECKBOX = 0x2,
              BS_AUTOCHECKBOX = 0x3, BS_RADIOBUTTON = 0x4, BS_USERBUTTON = 0x8,
              BS_AUTORADIOBUTTON = 0x9, BS_SPLITBUTTON = 0xC, BS_DEFSPLITBUTTON = 0xD,
              BS_COMMANDLINK = 0xE, BS_DEFCOMMANDLINK = 0xF;
constexpr u32 BS_MULTILINE = 0x2000;

constexpr u32 BUTTON_IMAGELIST_ALIGN_TOP = 2, BUTTON_IMAGELIST_ALIGN_BOTTOM = 3,
              BUTTON_IMAGELIST_ALIGN_CENTER = 4;

// DrawText flags (WinUser.h).
constexpr u32 DT_WORDBREAK = 0x10, DT_CALCRECT = 0x400, DT_EDITCONTROL = 0x2000;

bool is_button_class(const windowing::WindowData& win) {
    const std::wstring& c = win.control_class_name();
    if (c.size() != 6) return false;
    static constexpr wchar_t kButton[] = L"button";
    for (usize i = 0; i < 6; ++i) {
        wchar_t ch = c[i];
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
        if (ch != kButton[i]) return false;
    }
    return true;
}

bool is_command_link(u32 type) { return type == BS_COMMANDLINK || type == BS_DEFCOMMANDLINK; }

/// RECT / int[4] in guest memory.
bool read_rect(Marshaler& m, GuestAddr at, i32 (&r)[4]) {
    for (int i = 0; i < 4; ++i) {
        const auto v = m.read_u32(at + static_cast<u64>(i) * 4);
        if (v.is_err()) return false;
        r[i] = static_cast<i32>(v.value());
    }
    return true;
}
bool write_rect(Marshaler& m, GuestAddr at, const i32 (&r)[4]) {
    for (int i = 0; i < 4; ++i) {
        if (m.write_u32(at + static_cast<u64>(i) * 4, static_cast<u32>(r[i])).is_err()) return false;
    }
    return true;
}

/// Structure layouts (CommCtrl.h; probe "imagelist size" / "splitinfo size":
/// BUTTON_IMAGELIST 24 / 32 bytes and BUTTON_SPLITINFO 20 / 32 on x86 / x64).
struct ImageListLayout { u64 himl, margin, align; };
ImageListLayout imagelist_layout(bool x64) { return x64 ? ImageListLayout{0, 8, 24} : ImageListLayout{0, 4, 20}; }
struct SplitInfoLayout { u64 mask, glyph, style, size; };
SplitInfoLayout splitinfo_layout(bool x64) { return x64 ? SplitInfoLayout{0, 8, 16, 20} : SplitInfoLayout{0, 4, 8, 12}; }

}  // namespace

bool MessageSendManager::dispatch_button_control(Marshaler& m, windowing::HWND hwnd, u32 msg,
                                                 u64 wParam, u64 lParam, bool force_native) {
    const bool bcm = msg >= BCM_GETIDEALSIZE && msg <= BCM_SETSHIELD;
    if (!bcm && msg != BM_SETIMAGE && msg != BM_GETIMAGE) return false;
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win == nullptr || !is_button_class(*win)) return false;
    if (!force_native && win->wndproc != 0) return false;
    const bool x64 = m.cpu_is_64bit();
    const u32 type = win->style & BS_TYPEMASK;

    // BM_SETIMAGE / BM_GETIMAGE are user32's: every Button has them.
    if (msg == BM_SETIMAGE || msg == BM_GETIMAGE) {
        const u32 image_type = static_cast<u32>(wParam);
        std::lock_guard lock(m_control_state_mutex);
        ButtonExtState& st = m_button_ext_state[hwnd];
        u64* slot = image_type == IMAGE_BITMAP ? &st.bitmap
                    : image_type == IMAGE_ICON ? &st.icon : nullptr;
        if (slot == nullptr) {
            m.set_return(0);
            return true;
        }
        const u64 previous = *slot;
        if (msg == BM_SETIMAGE) {
            *slot = x64 ? lParam : (lParam & 0xFFFFFFFFull);
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!BM_SETIMAGE: the image is kept for "
                                     "BM_GETIMAGE / BCM_GETIDEALSIZE but not drawn on the native button\n");
            }
        }
        m.set_return(previous);
        return true;
    }

    // Without Common Controls 6.0 the Button is user32's, which has no BCM_*
    // messages: each returns 0 and writes nothing (probe "nov6").
    if (!m_common_controls_v6) {
        m.set_return(0);
        return true;
    }

    const GuestAddr p = static_cast<GuestAddr>(lParam);
    switch (msg) {
    case BCM_SETTEXTMARGIN:
    case BCM_GETTEXTMARGIN: {
        if (p == 0) { m.set_return(0); return true; }  // probe "null-lparam"
        std::lock_guard lock(m_control_state_mutex);
        ButtonExtState& st = m_button_ext_state[hwnd];
        bool ok = true;
        if (msg == BCM_SETTEXTMARGIN) {
            i32 r[4];
            ok = read_rect(m, p, r);
            if (ok) std::copy(std::begin(r), std::end(r), st.margin);  // negatives kept (probe "margin neg")
        } else {
            ok = write_rect(m, p, st.margin);
        }
        m.set_return(ok ? 1 : 0);
        return true;
    }
    case BCM_SETIMAGELIST:
    case BCM_GETIMAGELIST: {
        if (p == 0) { m.set_return(0); return true; }  // probe "imagelist ... null r=0"
        const ImageListLayout lay = imagelist_layout(x64);
        std::lock_guard lock(m_control_state_mutex);
        ButtonExtState& st = m_button_ext_state[hwnd];
        bool ok = true;
        if (msg == BCM_SETIMAGELIST) {
            const auto himl = m.read_ptr(p + lay.himl);
            const auto align = m.read_u32(p + lay.align);
            i32 r[4];
            ok = himl.is_ok() && align.is_ok() && read_rect(m, p + lay.margin, r);
            if (ok) {
                // Any uAlign is stored as given (probe "imagelist badalign r=1").
                st.image_list = himl.value();
                std::copy(std::begin(r), std::end(r), st.image_margin);
                st.image_align = align.value();
            }
        } else {
            ok = m.write_ptr(p + lay.himl, st.image_list).is_ok() &&
                 write_rect(m, p + lay.margin, st.image_margin) &&
                 m.write_u32(p + lay.align, st.image_align).is_ok();
        }
        m.set_return(ok ? 1 : 0);
        return true;
    }
    case BCM_SETDROPDOWNSTATE:
        // Measured: 0 for every button type and BM_GETSTATE shows no
        // BST_DROPDOWNPUSHED afterwards (probe "dropdown").
        m.set_return(0);
        return true;
    case BCM_SETSHIELD: {
        // Measured: returns 0 whether or not the state changes (probe "shield").
        std::lock_guard lock(m_control_state_mutex);
        m_button_ext_state[hwnd].shield = lParam != 0;
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comctl32!BCM_SETSHIELD: the shield is kept but "
                                 "not drawn on the native button\n");
        }
        m.set_return(0);
        return true;
    }
    case BCM_SETSPLITINFO:
    case BCM_GETSPLITINFO: {
        if (p == 0) { m.set_return(0); return true; }
        const SplitInfoLayout lay = splitinfo_layout(x64);
        const auto mask_r = m.read_u32(p + lay.mask);
        if (mask_r.is_err()) { m.set_return(0); return true; }
        const u32 mask = mask_r.value();
        std::lock_guard lock(m_control_state_mutex);
        ButtonExtState& st = m_button_ext_state[hwnd];
        if (msg == BCM_SETSPLITINFO) {
            // Accepted by every button type (probe "splitinfo push/cmdlink set r=1").
            if (mask & (BCSIF_GLYPH | BCSIF_IMAGE)) {
                const auto g = m.read_ptr(p + lay.glyph);
                if (g.is_ok()) { st.split_glyph = g.value(); st.split_glyph_set = true; }
            }
            if (mask & BCSIF_STYLE) {
                const auto s = m.read_u32(p + lay.style);
                if (s.is_ok()) st.split_style = s.value();
            }
            if (mask & BCSIF_SIZE) {
                const auto cx = m.read_u32(p + lay.size), cy = m.read_u32(p + lay.size + 4);
                if (cx.is_ok() && cy.is_ok()) {
                    st.split_size[0] = static_cast<i32>(cx.value());
                    st.split_size[1] = static_cast<i32>(cy.value());
                }
            }
        } else {
            if (mask & (BCSIF_GLYPH | BCSIF_IMAGE)) {
                if (!st.split_glyph_set) {
                    // Windows hands out its own glyph image list here
                    // (probe "glyph_null=0"); Vitreo has none to give.
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comctl32!BCM_GETSPLITINFO: the "
                                             "default drop-down glyph image list\n");
                    }
                }
                (void)m.write_ptr(p + lay.glyph, st.split_glyph);
            }
            if (mask & BCSIF_STYLE) (void)m.write_u32(p + lay.style, st.split_style);
            if (mask & BCSIF_SIZE) {
                (void)m.write_u32(p + lay.size, static_cast<u32>(st.split_size[0]));
                (void)m.write_u32(p + lay.size + 4, static_cast<u32>(st.split_size[1]));
            }
        }
        m.set_return(1);
        return true;
    }
    case BCM_SETNOTE:
    case BCM_GETNOTE:
    case BCM_GETNOTELENGTH: {
        // Notes belong to command links; other types answer 0 and, for the
        // set/get, ERROR_NOT_SUPPORTED (probe "note push/split").
        if (!is_command_link(type)) {
            if (msg != BCM_GETNOTELENGTH) set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(0);
            return true;
        }
        std::lock_guard lock(m_control_state_mutex);
        ButtonExtState& st = m_button_ext_state[hwnd];
        if (msg == BCM_GETNOTELENGTH) {
            m.set_return(st.note.size());
            return true;
        }
        if (msg == BCM_SETNOTE) {
            std::u16string text;
            for (u64 i = 0; p != 0 && i < 0x10000; ++i) {
                const auto u = m.read_u16(p + i * 2);
                if (u.is_err() || u.value() == 0) break;
                text.push_back(static_cast<char16_t>(u.value()));
            }
            st.note = std::move(text);
            m.set_return(1);
            return true;
        }
        // BCM_GETNOTE: wParam = DWORD* (buffer size in WCHARs), lParam = buffer.
        const GuestAddr pcch = static_cast<GuestAddr>(wParam);
        const auto cch_r = pcch != 0 ? m.read_u32(pcch) : VResult<u32>::ok(0);
        if (pcch == 0 || cch_r.is_err() || p == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return true;
        }
        const u32 cch = cch_r.value();
        const u32 needed = static_cast<u32>(st.note.size()) + 1;
        // A short buffer gets what fits plus a NUL, *pcch the size needed and
        // ERROR_INSUFFICIENT_BUFFER (probe "note cmdlink small": cch 3 -> 7).
        const u32 copy = std::min<u32>(static_cast<u32>(st.note.size()), cch == 0 ? 0 : cch - 1);
        for (u32 i = 0; i < copy; ++i) (void)m.write_u16(p + static_cast<u64>(i) * 2, st.note[i]);
        if (cch > 0) (void)m.write_u16(p + static_cast<u64>(copy) * 2, 0);
        if (cch < needed) {
            (void)m.write_u32(pcch, needed);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return true;
        }
        m.set_return(1);
        return true;
    }
    case BCM_GETIDEALSIZE:
        break;
    default:
        return false;
    }

    // ---- BCM_GETIDEALSIZE ---------------------------------------------------
    if (p == 0) { m.set_return(0); return true; }  // probe "null-lparam ideal r=0"
    const auto in_cx_r = m.read_u32(p);
    if (in_cx_r.is_err()) { m.set_return(0); return true; }
    const i32 in_cx = static_cast<i32>(in_cx_r.value());
    auto put = [&](i32 cx, i32 cy) {
        (void)m.write_u32(p, static_cast<u32>(cx));
        (void)m.write_u32(p + 4, static_cast<u32>(cy));
        m.set_return(1);
        return true;
    };
    const i32 win_w = win->rect.width(), win_h = win->rect.height();

    if (is_command_link(type)) {
        // A command link is laid out with the theme's title and note fonts
        // and its arrow glyph, independent of WM_SETFONT (probe "cmdlink").
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comctl32!BCM_GETIDEALSIZE: command link "
                                 "layout (theme fonts and glyph)\n");
        }
        m.set_return(0);
        return true;
    }
    const bool push_like = type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON ||
                           type == BS_USERBUTTON || type == BS_SPLITBUTTON ||
                           type == BS_DEFSPLITBUTTON;
    const bool check_like = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
                            type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    // 3-state boxes, group boxes, owner-draw and push boxes report their
    // window rectangle (Learn: "any other type"; probe "3state/group/owner/
    // pushbox/auto3state").
    if (!push_like && !check_like) return put(win_w, win_h);

    ButtonExtState st;
    {
        std::lock_guard lock(m_control_state_mutex);
        st = m_button_ext_state[hwnd];
    }
    // The window text; the native control holds it once one exists.
    std::wstring wtext = get_control_text(hwnd);
    if (wtext.empty()) wtext = win->title;
    // Empty text reports the window rectangle (probe ".../empty").
    if (wtext.empty()) return put(win_w, win_h);
    if (m_gdi_manager == nullptr) { m.set_return(0); return true; }

    const bool def = type == BS_DEFPUSHBUTTON || type == BS_DEFSPLITBUTTON;
    const bool split = type == BS_SPLITBUTTON || type == BS_DEFSPLITBUTTON;
    GdiManager& gdi = *m_gdi_manager;
    const Handle dc = gdi.create_compatible_dc(0);
    if (dc == 0) { m.set_return(0); return true; }
    // The control's WM_SETFONT font, or the system font a Button starts with
    // (probe "sysfont" rows).
    const Handle font = win->hfont != 0 ? static_cast<Handle>(win->hfont)
                                        : gdi.get_stock_object(SYSTEM_FONT_ID);
    const Handle old_font = gdi.select_object(dc, font);
    std::u16string text(wtext.begin(), wtext.end());

    // Fixed parts at 96 DPI (probe): a push button adds 5 x 7 around the
    // margined text (a default button 2 more each way, a split button its
    // 32-pixel drop-down part); a check box or radio button adds its 13-pixel
    // glyph, 5 pixels and half the width of "0" (probe "checkgap").
    i32 kx = 5, ky = 7;
    if (def) { kx += 2; ky += 2; }
    if (split) kx += 32;
    if (check_like) {
        i32 zero_w = 0, zero_h = 0;
        (void)gdi.text_extent(dc, u"0", zero_w, zero_h);
        kx = 13 + 5 + zero_w / 2;
    }
    const i32 mx = st.margin[0] + st.margin[2];
    const i32 my = st.margin[1] + st.margin[3];

    // The text box is DrawText's DT_CALCRECT box: '&' prefixes are removed
    // and "\n" breaks lines whatever the style (probe ".../amp", ".../twolines").
    // A BS_MULTILINE button given a width wraps its text to that width less
    // the fixed parts, breaking long words as an edit control does (probe
    // "incx"); without BS_MULTILINE the width is ignored.
    u32 format = DT_CALCRECT;
    i32 left = 0, top = 0, right = 0, bottom = 0;
    bool wrap = (win->style & BS_MULTILINE) != 0 && in_cx > 0;
    i32 avail = 0;
    if (wrap) {
        avail = in_cx - mx - kx;
        format |= DT_WORDBREAK | DT_EDITCONTROL;
        right = avail;
    }
    i32 tw = 0, th = 0;
    if (!wrap || avail > 0) {
        (void)gdi.draw_text_ex(dc, text, left, top, right, bottom, format, nullptr);
        tw = right - left;
        th = bottom - top;
    }
    (void)gdi.select_object(dc, old_font);
    (void)gdi.delete_dc(dc);

    if (wrap && avail <= 0) {
        // No room for any text: Windows answers one pixel wider than asked
        // with the bare height (probe "incx1"/"incx10": 7 for a push button,
        // the 13-pixel glyph for a check box).
        return put(in_cx + 1, check_like ? 13 : ky);
    }

    i32 cx = tw + mx + kx;
    i32 cy = th + my + ky;
    // An image list (BCM_SETIMAGELIST) sits beside or above/below the text
    // with its margins, or alone when centred; a BM_SETIMAGE bitmap sits
    // beside the text with no margin (probe "imagelist align=", "typeimagelist",
    // "bmsetimage").
    if (st.image_list != 0) {
        i32 iw = 0, ih = 0;
        if (!m_image_list_size_query || !m_image_list_size_query(st.image_list, iw, ih)) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comctl32!BCM_GETIDEALSIZE: size of an "
                                     "image list this process did not create through comctl32\n");
            }
            m.set_return(0);
            return true;
        }
        iw += st.image_margin[0] + st.image_margin[2];
        ih += st.image_margin[1] + st.image_margin[3];
        if (st.image_align == BUTTON_IMAGELIST_ALIGN_CENTER) return put(iw, ih);
        if (st.image_align == BUTTON_IMAGELIST_ALIGN_TOP || st.image_align == BUTTON_IMAGELIST_ALIGN_BOTTOM) {
            cx = std::max(cx, iw);
            cy += ih;
        } else {
            cx += iw;
            cy = std::max(cy, ih);
        }
    } else if (st.bitmap != 0 || st.icon != 0) {
        i32 iw = 0, ih = 0;
        bool known = false;
        if (st.bitmap != 0) {
            GdiManager::BitmapRgbaCopy copy;
            known = m_gdi_manager->copy_bitmap_rgba(static_cast<Handle>(st.bitmap), copy);
            iw = copy.width;
            ih = copy.height;
        } else if (m_icon_size_query) {
            known = m_icon_size_query(st.icon, iw, ih);
        }
        if (!known) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!BCM_GETIDEALSIZE: size of a "
                                     "BM_SETIMAGE handle Vitreo does not know\n");
            }
            m.set_return(0);
            return true;
        }
        cx += iw;
        cy = std::max(cy, ih);
    }
    return put(cx, cy);
}

}  // namespace vitreo::winapi
