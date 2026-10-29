// SPDX-License-Identifier: LGPL-2.1-only

/// @file user32_ext_registration.cpp
/// @brief Export table for user32's extended surface.
///
/// Composition only: maps export names to User32ExtManager handlers and records
/// each one's stdcall byte count. No window, menu, or clipboard behavior lives
/// here — every handler forwards to the manager, or is a small lambda belonging
/// to the export it registers.
///
/// Split out of user32_ext.cpp, which was 11,454 lines and roughly a quarter
/// registration. Adding an export no longer touches the translation unit where
/// window, menu, dialog, and clipboard behavior lives.
///
/// The table body is split across registration/exports_*.cpp in registration
/// order.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.

#include "winapi/user32/registration/registration_parts.hpp"
#include "winapi/user32/user32_ext.hpp"

namespace vitreo::winapi {

// ---------- Thunk registration ----------

DllExports create_user32_ext_thunks(User32ExtManager& mgr) {
    DllExports exports;
    exports.entries.reserve(160);

    add_user32_ext_exports_core(exports, mgr);
    add_user32_ext_exports_window_objects(exports, mgr);
    add_user32_ext_exports_focus_cursor(exports, mgr);
    add_user32_ext_exports_input_menu_scroll(exports, mgr);
    add_user32_ext_exports_icon_clipboard_window(exports, mgr);
    add_user32_ext_exports_pointer_display_rawinput(exports, mgr);
    add_user32_ext_exports_caret_keyboard_layout(exports, mgr);
    add_user32_ext_exports_dpi_displayconfig(exports, mgr);
    add_user32_ext_exports_audit(exports, mgr);

    return exports;
}

} // namespace vitreo::winapi
