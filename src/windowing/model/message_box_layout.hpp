// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file message_box_layout.hpp
/// @brief MessageBox button set, default button and icon from uType.
///
/// Values from winuser.h (10.0.26100); behaviour from Learn
/// (nf-winuser-messageboxw): the button group in bits 0-3, the icon in bits
/// 4-7, MB_DEFBUTTON1..4 in bits 8-11, MB_HELP adds a Help button after the
/// group (it sends WM_HELP and never closes the box).

#include <vitreo/common.hpp>

namespace vitreo::windowing::message_box {

constexpr u32 MB_TYPEMASK = 0x0000000F;
constexpr u32 MB_ICONMASK = 0x000000F0;
constexpr u32 MB_DEFMASK = 0x00000F00;
constexpr u32 MB_HELP = 0x00004000;

constexpr u32 MB_ICONHAND = 0x00000010;         // == MB_ICONERROR / MB_ICONSTOP
constexpr u32 MB_ICONQUESTION = 0x00000020;
constexpr u32 MB_ICONEXCLAMATION = 0x00000030;  // == MB_ICONWARNING
constexpr u32 MB_ICONASTERISK = 0x00000040;     // == MB_ICONINFORMATION

constexpr i32 IDOK = 1, IDCANCEL = 2, IDABORT = 3, IDRETRY = 4, IDIGNORE = 5,
              IDYES = 6, IDNO = 7, IDTRYAGAIN = 10, IDCONTINUE = 11;
/// Not a MessageBox return value: marks the MB_HELP button.
constexpr i32 IDHELP_BUTTON = 9;

enum class Icon : u8 { None, Error, Question, Warning, Information };

struct Button {
    i32 id;
    const char* title;
};

struct Layout {
    Button buttons[4]{};
    u32 count = 0;
    u32 default_index = 0;  ///< index into buttons (0-based)
    Icon icon = Icon::None;
    bool has_cancel = false;  ///< Esc / close returns IDCANCEL
};

[[nodiscard]] constexpr Layout layout(u32 type) noexcept {
    Layout l;
    auto add = [&l](i32 id, const char* title) { l.buttons[l.count++] = {id, title}; };
    switch (type & MB_TYPEMASK) {
    case 0x1: add(IDOK, "OK"); add(IDCANCEL, "Cancel"); break;
    case 0x2: add(IDABORT, "Abort"); add(IDRETRY, "Retry"); add(IDIGNORE, "Ignore"); break;
    case 0x3: add(IDYES, "Yes"); add(IDNO, "No"); add(IDCANCEL, "Cancel"); break;
    case 0x4: add(IDYES, "Yes"); add(IDNO, "No"); break;
    case 0x5: add(IDRETRY, "Retry"); add(IDCANCEL, "Cancel"); break;
    case 0x6: add(IDCANCEL, "Cancel"); add(IDTRYAGAIN, "Try Again"); add(IDCONTINUE, "Continue"); break;
    default: add(IDOK, "OK"); break;
    }
    for (u32 i = 0; i < l.count; ++i) {
        if (l.buttons[i].id == IDCANCEL) l.has_cancel = true;
    }
    if ((type & MB_HELP) != 0) add(IDHELP_BUTTON, "Help");
    const u32 def = (type & MB_DEFMASK) >> 8;
    l.default_index = def < l.count ? def : 0;
    switch (type & MB_ICONMASK) {
    case MB_ICONHAND: l.icon = Icon::Error; break;
    case MB_ICONQUESTION: l.icon = Icon::Question; break;
    case MB_ICONEXCLAMATION: l.icon = Icon::Warning; break;
    case MB_ICONASTERISK: l.icon = Icon::Information; break;
    default: break;
    }
    return l;
}

/// The result an unattended (headless) box gives: its default button, or the
/// first real button when the default is the Help button.
[[nodiscard]] constexpr i32 default_result(u32 type) noexcept {
    const Layout l = layout(type);
    const i32 id = l.buttons[l.default_index].id;
    return id == IDHELP_BUTTON ? l.buttons[0].id : id;
}

} // namespace vitreo::windowing::message_box
