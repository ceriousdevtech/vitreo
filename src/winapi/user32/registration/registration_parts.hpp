// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

/// @file registration_parts.hpp
/// @brief Private to user32_ext_registration.cpp: one declaration per slice of
///        the former create_user32_ext_thunks body.
///
/// The user32 extended export table is registered in a fixed order, which
/// decides thunk-address allocation. create_user32_ext_thunks calls these in
/// that order; each appends its contiguous run of entries to `exports`.

#include "winapi/user32/user32_ext.hpp"

namespace vitreo::winapi {

void add_user32_ext_exports_core(DllExports& exports, User32ExtManager& mgr);
void add_user32_ext_exports_window_objects(DllExports& exports, User32ExtManager& mgr);
void add_user32_ext_exports_focus_cursor(DllExports& exports, User32ExtManager& mgr);
void add_user32_ext_exports_input_menu_scroll(DllExports& exports, User32ExtManager& mgr);
void add_user32_ext_exports_icon_clipboard_window(DllExports& exports, User32ExtManager& mgr);
void add_user32_ext_exports_pointer_display_rawinput(DllExports& exports, User32ExtManager& mgr);
void add_user32_ext_exports_caret_keyboard_layout(DllExports& exports, User32ExtManager& mgr);
void add_user32_ext_exports_dpi_displayconfig(DllExports& exports, User32ExtManager& mgr);
void add_user32_ext_exports_audit(DllExports& exports, User32ExtManager& mgr);

} // namespace vitreo::winapi
