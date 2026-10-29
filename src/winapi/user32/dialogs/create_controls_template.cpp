// SPDX-License-Identifier: LGPL-2.1-only

/// @file dialog.cpp
/// @brief Dialog box management implementation — template-driven

#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/dialogs/dialog_internal.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/parser/pe_structures.hpp"


#include <cstdio>
#include <cstdlib>
#include "windowing/model/window_types.hpp"

#include <cmath>
#include <cstdlib>
#include <unistd.h>  // usleep
#include <optional>
#include <algorithm>
#include <array>

/// @file create_controls_template.cpp
/// @brief create_controls_from_template
///
/// Closed cut from dialog.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// create_controls_from_template
// =============================================================================

void DialogManager::create_controls_from_template(
        windowing::HWND dlg_hwnd,
        const windowing::DialogTemplate& tmpl,
        GuestAddr rsrc_image_base,
        u32 rsrc_rva,
        u32 rsrc_size) {

    // Compute DLU base units from the dialog template font
    // Without DS_SETFONT the dialog uses the system font: GetDialogBaseUnits'
        // 8 x 16 (measured, s1win_misc_probe mapdialogrect: 4,8 DLU -> 8,16;
        // an 8-point MS Shell Dlg font gives 6 x 13).
        i32 base_x = 8, base_y = 16;
    if ((tmpl.style & windowing::DS::SETFONT) && tmpl.font_size > 0) {
        windowing::compute_dlu_bases(tmpl.font_size, base_x, base_y);
    }

    // Dialog font info for applying to each control.
    // Windows font_size is in Win32 points (at 96 DPI).  The macOS system font
    // (.AppleSystemUIFont) is wider per character than Tahoma / MS Shell Dlg,
    // so we use a 1.25x factor instead of the exact 96/72 (1.333x) to avoid
    // text overflowing control boundaries.  Rounding to integer ensures crisp
    // rendering on both standard and Retina displays.
    const bool have_font = (tmpl.style & windowing::DS::SETFONT) && tmpl.font_size > 0;
    const float font_pt  = have_font ? std::roundf(tmpl.font_size * 1.375f) : 13.0f;
    const u16 font_wt    = have_font ? tmpl.font_weight : 400;
    const bool font_ital = have_font && tmpl.font_italic != 0;

    for (const auto& item : tmpl.items) {
        // Convert DLU positions to pixel positions using computed bases
        i32 px_x  = windowing::dlu_to_px_x(item.x, base_x);
        i32 px_y  = windowing::dlu_to_px_y(item.y, base_y);
        i32 px_cx = windowing::dlu_to_px_x(item.cx, base_x);
        i32 px_cy = windowing::dlu_to_px_y(item.cy, base_y);

        windowing::WindowData child{};
        child.style      = item.style | windowing::WS::CHILD;
        child.ex_style   = item.ex_style;
        child.parent     = dlg_hwnd;
        child.title      = item.title;
        child.rect       = windowing::Rect{px_x, px_y, px_x + px_cx, px_y + px_cy};
        child.client_rect = child.rect;
        child.visible    = (item.style & windowing::WS::VISIBLE) != 0;
        child.enabled    = !(item.style & windowing::WS::DISABLED);
        // GWL_USERDATA starts at 0 (Learn: GetWindowLongPtrW); the control ID
        // lives in ctrl_id only.
        child.ctrl_id    = static_cast<u32>(item.id);   // separate control ID field
        child.wndproc    = 0;   // default control proc
        // MS Learn system class names (mixed case). GetClassName returns
        // "Button", "Static", and the other system-class names.
        if (!item.class_name.empty()) {
            child.class_name = item.class_name;
        } else {
            switch (item.class_ordinal) {
                case 0x0080: child.class_name = L"Button"; break;
                case 0x0081: child.class_name = L"Edit"; break;
                case 0x0082: child.class_name = L"Static"; break;
                case 0x0083: child.class_name = L"ListBox"; break;
                case 0x0084: child.class_name = L"ScrollBar"; break;
                case 0x0085: child.class_name = L"ComboBox"; break;
                default:
                    if (item.class_ordinal != 0) {
                        child.class_name =
                            L"#" + std::to_wstring(item.class_ordinal);
                    }
                    break;
            }
        }
        child.class_atom = item.class_ordinal;
        // The window belongs to the class the template names (a system class
        // for the ordinals): GCW_ATOM, cbWndExtra and GetClassLong read it.
        if (const WindowClassData* cc = m_class_mgr.find_by_name(child.class_name)) {
            child.class_id = cc->id;
            child.class_style = cc->style;
            child.wnd_extra_size = static_cast<u32>(std::max(0, cc->wnd_extra));
            child.extra_bytes.assign(child.wnd_extra_size, 0);
        }
        if (const windowing::WindowData* dlg = m_hwnd_mgr.find(dlg_hwnd)) {
            child.hinstance = dlg->hinstance;
            child.owner_thread_id = dlg->owner_thread_id;
        }

        windowing::HWND child_hwnd = m_hwnd_mgr.create(child);

        // Track in dialog item map so GetDlgItem can find it
        DlgItemKey key{dlg_hwnd, item.id};
        m_dlg_items[key] = child_hwnd;

        // Determine the control class name for the native view
        u16 cls_ord = item.class_ordinal;

        VITREO_TRACE(
            "[dialog]   control id=%u class=0x%04x \"%ls\" "
            "pos=(%d,%d) size=(%dx%d) style=0x%08x visible=%s title_ord=%u\n",
            item.id, cls_ord, item.title.c_str(),
            px_x, px_y, px_cx, px_cy, item.style,
            child.visible ? "yes" : "no",
            static_cast<unsigned>(item.title_ordinal));

        // Create native Cocoa child view
        m_control_host.add_child_view(dlg_hwnd, child_hwnd, child,
                                cls_ord, item.class_name, item.title);

        // For SS_ICON controls with a resource ordinal title, load the icon
        // from the PE resources and set it on the NSImageView.
        constexpr u32 SS_ICON = 0x03;
        constexpr u32 SS_BITMAP = 0x0E;
        if (cls_ord == 0x0082 && (item.style & 0x1F) == SS_ICON && item.title_ordinal != 0) {
            auto ico_data = windowing::extract_icon_by_group_id(
                m_memory, rsrc_image_base, rsrc_rva, rsrc_size,
                item.title_ordinal);
            if (!ico_data.empty()) {
                m_control_host.set_control_image_data(
                    child_hwnd, ico_data.data(), ico_data.size());
                VITREO_TRACE( "[dialog]   -> set icon from group_id=%u (%zu bytes)\n",
                             item.title_ordinal, ico_data.size());
            } else {
                VITREO_TRACE( "[dialog]   -> FAILED to load icon group_id=%u\n",
                             item.title_ordinal);
            }
        } else if (cls_ord == 0x0082 && (item.style & 0x1F) == SS_BITMAP) {
            bool bmp_loaded = false;

            if (item.title_ordinal != 0) {
                auto bmp_res = windowing::find_resource(
                    m_memory, rsrc_image_base, rsrc_rva, rsrc_size,
                    windowing::RT::BITMAP, item.title_ordinal);
                if (bmp_res.data && bmp_res.size > 0) {
                    auto bmp_file = dib_to_bmp_file(bmp_res.data, bmp_res.size);
                    if (!bmp_file.empty() &&
                        m_control_host.set_control_image_data(
                            child_hwnd, bmp_file.data(), bmp_file.size())) {
                        VITREO_TRACE("[dialog]   -> set bitmap id=%u (%zu bytes DIB, %zu bytes BMP)\n",
                                     item.title_ordinal,
                                     static_cast<size_t>(bmp_res.size),
                                     bmp_file.size());
                        bmp_loaded = true;
                    } else {
                        VITREO_TRACE("[dialog]   -> FAILED to set bitmap id=%u\n",
                                     item.title_ordinal);
                    }
                } else {
                    VITREO_TRACE("[dialog]   -> bitmap resource not found id=%u "
                                 "(module=0x%llx rva=0x%x)\n",
                                 item.title_ordinal,
                                 static_cast<unsigned long long>(rsrc_image_base),
                                 rsrc_rva);
                }
            }

            if (!bmp_loaded && !item.title.empty()) {
                std::u16string name16;
                name16.reserve(item.title.size());
                for (wchar_t ch : item.title) {
                    name16.push_back(static_cast<char16_t>(ch & 0xFFFF));
                }

                auto bmp_res = windowing::find_resource_by_string_name(
                    m_memory, rsrc_image_base, rsrc_rva, rsrc_size,
                    windowing::RT::BITMAP,
                    name16.data(), static_cast<u16>(name16.size()));
                if (bmp_res.data && bmp_res.size > 0) {
                    auto bmp_file = dib_to_bmp_file(bmp_res.data, bmp_res.size);
                    if (!bmp_file.empty() &&
                        m_control_host.set_control_image_data(
                            child_hwnd, bmp_file.data(), bmp_file.size())) {
                        VITREO_TRACE("[dialog]   -> set bitmap name=\"%s\" (%zu bytes DIB, %zu bytes BMP)\n",
                                     wstring_to_utf8(item.title).c_str(),
                                     static_cast<size_t>(bmp_res.size),
                                     bmp_file.size());
                        bmp_loaded = true;
                    }
                }
            }

            if (!bmp_loaded) {
                // A deferred SS_BITMAP has neither a resource id nor a name in the
                // dialog template — the bitmap is attached at runtime via
                // STM_SETIMAGE after LoadImageW. That is not an error, so
                // demote the log to a trace breadcrumb. Only flag a real
                // warning when the template references a specific resource
                // that could not be found.
                if (item.title_ordinal == 0 && item.title.empty()) {
                    VITREO_TRACE("[dialog]   -> SS_BITMAP placeholder (template has no image; awaiting STM_SETIMAGE)\n");
                } else {
                    VITREO_TRACE("[dialog]   -> SS_BITMAP unresolved (id=%u name=\"%s\")\n",
                                 item.title_ordinal,
                                 wstring_to_utf8(item.title).c_str());
                }
            }
        }

        // Apply dialog template font to the control (matches Win32 behavior
        // where CreateDialog sends WM_SETFONT to every child control)
        if (have_font) {
            m_control_host.set_control_font(
                child_hwnd, font_pt, font_wt, font_ital);
        }
    }
}

} // namespace vitreo::winapi
