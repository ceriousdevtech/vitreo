// SPDX-License-Identifier: LGPL-2.1-only

/// @file create_dialog.cpp
/// @brief DialogManager::create_dialog — template-driven creation.
///
/// Closed method cut from dialog.cpp.

#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/dialogs/dialog_internal.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "windowing/model/window_types.hpp"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <unistd.h>
#include <optional>
#include <algorithm>
#include <array>

namespace vitreo::winapi {

windowing::HWND DialogManager::create_dialog(u64 hinstance,
                                             u64 template_id,
                                             windowing::HWND parent,
                                             u64 dlgproc,
                                             u64 init_param,
                                             bool post_init,
                                             bool require_template,
                                             bool modal) {
    ensure_dialog_class();

    // Resolve parent — use desktop if null
    if (parent == windowing::NULL_HWND) {
        parent = windowing::HWND_DESKTOP;
    }

    // Resolve parent alias: if the parent HWND is unknown, map it to a
    // known HWND. A caller may pass a stored HWND that is not in
    // HwndManager.
    if (parent != windowing::HWND_DESKTOP && !m_hwnd_mgr.is_valid(parent)) {
        auto alias_it = m_hwnd_aliases.find(parent);
        if (alias_it != m_hwnd_aliases.end()) {
            VITREO_TRACE( "[dialog] create_dialog: resolving parent alias 0x%x -> 0x%x\n",
                         parent, alias_it->second);
            parent = alias_it->second;
        } else if (m_current_modal_hwnd != windowing::NULL_HWND
                   && m_hwnd_mgr.is_valid(m_current_modal_hwnd)) {
            VITREO_TRACE( "[dialog] create_dialog: mapping unknown parent 0x%x -> modal 0x%x\n",
                         parent, m_current_modal_hwnd);
            m_hwnd_aliases[parent] = m_current_modal_hwnd;
            parent = m_current_modal_hwnd;
        }
    }

    // ---- Try to load the real dialog template from the PE resource section ----
    windowing::DialogTemplate tmpl{};
    bool have_template = false;

    // Resolve resource directory: prefer the passed-in hinstance/module base if present.
    GuestAddr rsrc_image_base = m_image_base;
    u32 rsrc_rva = m_rsrc_rva;
    u32 rsrc_size = m_rsrc_size;

    if (hinstance != 0 && static_cast<GuestAddr>(hinstance) != m_image_base) {
        if (auto info = try_get_resource_dir_from_module(m_memory, static_cast<GuestAddr>(hinstance))) {
            rsrc_image_base = info->image_base;
            rsrc_rva = info->rsrc_rva;
            rsrc_size = info->rsrc_size;
            VITREO_TRACE(
                "[dialog] create_dialog: using resources from hinstance=0x%llx (RVA=0x%x size=0x%x)\n",
                static_cast<unsigned long long>(hinstance), rsrc_rva, rsrc_size);
        } else {
            VITREO_TRACE(
                "[dialog] create_dialog: could not derive .rsrc from hinstance=0x%llx; falling back to main module\n",
                static_cast<unsigned long long>(hinstance));
        }
    }

    if (rsrc_rva != 0 && rsrc_size != 0 && template_id <= 0xFFFF) {
        auto res = windowing::find_resource(
            m_memory, rsrc_image_base, rsrc_rva, rsrc_size,
            windowing::RT::DIALOG, static_cast<u16>(template_id));

        if (res.data && res.size > 0) {
            auto parsed = windowing::parse_dialog_template(res.data, res.size);
            if (parsed.is_ok()) {
                tmpl = std::move(parsed.value());
                have_template = true;
                VITREO_TRACE(
                    "[dialog] Parsed template id=%llu: \"%ls\" %dx%d DLU, "
                    "%u items, style=0x%08x\n",
                    static_cast<unsigned long long>(template_id),
                    tmpl.title.c_str(),
                    static_cast<int>(tmpl.cx), static_cast<int>(tmpl.cy),
                    static_cast<unsigned>(tmpl.item_count),
                    tmpl.style);
            } else {
                VITREO_TRACE(
                    "[dialog] Template parse failed for id=%llu\n",
                    static_cast<unsigned long long>(template_id));
            }
        } else {
            VITREO_TRACE(
                "[dialog] Resource not found: type=RT_DIALOG, id=%llu\n",
                static_cast<unsigned long long>(template_id));
        }
    }

    // DialogBoxParam / CreateDialogParam take the template's resource NAME
    // when it is not MAKEINTRESOURCE (Learn: lpTemplateName).
    if (!have_template && !require_template && template_id > 0xFFFF && rsrc_rva != 0) {
        std::u16string name;
        const GuestAddr name_ptr = static_cast<GuestAddr>(template_id);
        for (u32 i = 0; i < 256; ++i) {
            u16 ch = 0;
            if (t_template_name_ansi) {
                ch = m_memory.read_u8(name_ptr + i).value_or(0);
            } else {
                ch = m_memory.read_u16(name_ptr + i * 2).value_or(0);
            }
            if (ch == 0) break;
            if (ch >= u'a' && ch <= u'z') ch = static_cast<u16>(ch - (u'a' - u'A'));
            name.push_back(static_cast<char16_t>(ch));
        }
        if (!name.empty()) {
            auto res = windowing::find_resource_by_string_name(
                m_memory, rsrc_image_base, rsrc_rva, rsrc_size, windowing::RT::DIALOG,
                name.c_str(), static_cast<u16>(name.size()));
            if (res.data && res.size > 0) {
                auto parsed = windowing::parse_dialog_template(res.data, res.size);
                if (parsed.is_ok()) {
                    tmpl = std::move(parsed.value());
                    have_template = true;
                }
            }
            if (!have_template) {
                set_last_error(1814);  // ERROR_RESOURCE_NAME_NOT_FOUND
                return windowing::NULL_HWND;
            }
        }
    }

    // CreateDialogIndirectParamW commonly passes an in-memory DLGTEMPLATE
    // pointer instead of a resource id.
    if (!have_template && template_id > 0xFFFF && require_template) {
        auto tmpl_addr = static_cast<GuestAddr>(template_id);
        if (try_parse_dialog_from_guest_memory(m_memory, tmpl_addr, tmpl)) {
            have_template = true;
            VITREO_TRACE(
                "[dialog] Parsed in-memory template @0x%llx: \"%ls\" %dx%d DLU, %u items, style=0x%08x\n",
                static_cast<unsigned long long>(template_id),
                tmpl.title.c_str(),
                static_cast<int>(tmpl.cx), static_cast<int>(tmpl.cy),
                static_cast<unsigned>(tmpl.item_count),
                tmpl.style);
        } else {
            VITREO_TRACE(
                "[dialog] Failed to parse in-memory template @0x%llx\n",
                static_cast<unsigned long long>(template_id));
        }
    }

    if (require_template && !have_template) {
        set_last_error(error_codes::ERROR_INVALID_DATA);
        VITREO_TRACE(
            "[dialog] Required in-memory template is invalid @0x%llx\n",
            static_cast<unsigned long long>(template_id));
        return windowing::NULL_HWND;
    }
    // A template that cannot be found fails like FindResource: no resource
    // section is ERROR_RESOURCE_DATA_NOT_FOUND, no such dialog
    // ERROR_RESOURCE_NAME_NOT_FOUND (measured, s1win_misc_probe
    // dialogboxparam_missing_resource: -1 / 1812 for an image without
    // resources). Nothing is shown in its place.
    if (!have_template) {
        constexpr u32 ERROR_RESOURCE_DATA_NOT_FOUND = 1812;
        constexpr u32 ERROR_RESOURCE_NAME_NOT_FOUND = 1814;
        set_last_error(rsrc_rva == 0 || rsrc_size == 0 ? ERROR_RESOURCE_DATA_NOT_FOUND
                                                       : ERROR_RESOURCE_NAME_NOT_FOUND);
        return windowing::NULL_HWND;
    }

    // ---- Build WindowData from template (or fallback to hardcoded defaults) ----
    windowing::WindowData data{};
    data.is_dialog = true;

    if (have_template) {
        // Learn CreateDialogParam: CreateWindowEx, then WM_INITDIALOG, then
        // display only if the template has WS_VISIBLE. Do not OR VISIBLE here
        // or the first NSWindow show happens before controls exist and before
        // INITDIALOG can SetWindowPos (parked/centered installer pages).
        data.style    = tmpl.style;
        data.ex_style = tmpl.ex_style;
        data.title    = tmpl.title;

        // Without DS_SETFONT the dialog uses the system font: GetDialogBaseUnits'
        // 8 x 16 (measured, s1win_misc_probe mapdialogrect: 4,8 DLU -> 8,16;
        // an 8-point MS Shell Dlg font gives 6 x 13).
        i32 base_x = 8, base_y = 16;
        if ((tmpl.style & windowing::DS::SETFONT) && tmpl.font_size > 0) {
            windowing::compute_dlu_bases(tmpl.font_size, base_x, base_y);
        }

        i32 px_w = windowing::dlu_to_px_x(tmpl.cx, base_x);
        i32 px_h = windowing::dlu_to_px_y(tmpl.cy, base_y);
        i32 px_x = windowing::dlu_to_px_x(tmpl.x, base_x);
        i32 px_y = windowing::dlu_to_px_y(tmpl.y, base_y);

        // Enforce minimum visible size
        if (px_w < 100) px_w = DEFAULT_DLG_WIDTH;
        if (px_h < 60)  px_h = DEFAULT_DLG_HEIGHT;

        data.rect = windowing::Rect{px_x, px_y, px_x + px_w, px_y + px_h};
    } else {
        // Fallback: hardcoded defaults
        data.style    = windowing::WS::POPUP | windowing::WS::CAPTION
                      | windowing::WS::SYSMENU | windowing::WS::VISIBLE;
        data.ex_style = 0;
        data.title    = L"Dialog";
        data.rect     = windowing::Rect{DEFAULT_DLG_X, DEFAULT_DLG_Y,
                                        DEFAULT_DLG_X + DEFAULT_DLG_WIDTH,
                                        DEFAULT_DLG_Y + DEFAULT_DLG_HEIGHT};
    }

    // MS Learn CreateDialog*: hwndParent is the owner for top-level dialogs
    // and the parent for WS_CHILD dialogs. Match WindowManager::create_window
    // so GetParent/GetWindowRect/MapWindowPoints do not walk an owned popup
    // as if it were a child (double-offset screen coordinates).
    const bool style_is_child = (data.style & windowing::WS::CHILD) != 0;
    if (!style_is_child && parent != windowing::NULL_HWND &&
        parent != windowing::HWND_DESKTOP) {
        data.owner  = parent;
        data.parent = windowing::HWND_DESKTOP;
    } else {
        data.parent = parent;
        data.owner  = windowing::NULL_HWND;
    }
    data.owner_thread_id = vitreo::winapi::ThreadManager::get_current_thread_id_static();
    data.hinstance   = hinstance;
    data.wndproc     = dlgproc;
    data.client_rect = data.rect;
    const bool template_visible = (data.style & windowing::WS::VISIBLE) != 0;
    data.style &= ~windowing::WS::VISIBLE;
    data.visible     = false;
    data.enabled     = true;

    // MS Learn DLGTEMPLATE: the template x/y are dialog units measured from
    // the upper-left corner of the *owner window's client area*, unless the
    // dialog carries DS_ABSALIGN, in which case they are screen coordinates.
    // Treating them as screen coordinates parks a (0,0) template — the common
    // case — in the desktop corner instead of over its owner.
    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-dlgtemplate
    if (have_template && !style_is_child &&
        (tmpl.style & windowing::DS::ABSALIGN) == 0) {
        const windowing::HWND anchor =
            (data.owner != windowing::NULL_HWND) ? data.owner : parent;
        if (anchor != windowing::NULL_HWND &&
            anchor != windowing::HWND_DESKTOP) {
            if (const windowing::WindowData* a = m_hwnd_mgr.find(anchor)) {
                const i32 ox = a->rect.left + a->client_offset.x;
                const i32 oy = a->rect.top + a->client_offset.y;
                const i32 dw = data.rect.right - data.rect.left;
                const i32 dh = data.rect.bottom - data.rect.top;
                data.rect = windowing::Rect{data.rect.left + ox,
                                            data.rect.top + oy,
                                            data.rect.right + ox,
                                            data.rect.bottom + oy};
                data.client_rect = windowing::Rect{0, 0, dw, dh};
                VITREO_TRACE(
                    "[dialog] template origin is owner-client relative: "
                    "anchor=0x%x -> screen (%d,%d)\n",
                    anchor, data.rect.left, data.rect.top);
            }
        }
    }

    // DS_CENTER centers in the owner's MONITOR WORK AREA, not its window.
    // https://learn.microsoft.com/en-us/windows/win32/dlgbox/dialog-box-styles
    if (have_template && (tmpl.style & windowing::DS::CENTER)) {
        auto mon = m_control_host.get_primary_monitor_info();
        const auto owner = data.owner != windowing::NULL_HWND ? data.owner : parent;
        const auto* owner_data = m_hwnd_mgr.find(owner);
        if (m_user32_ext_mgr && owner_data && owner != windowing::HWND_DESKTOP) {
            auto rect = owner_data->rect;
            auto ancestor = owner_data;
            while ((ancestor->style & windowing::WS::CHILD) != 0) {
                ancestor = m_hwnd_mgr.find(ancestor->parent);
                if (!ancestor) break;
                rect.left += ancestor->rect.left;
                rect.right += ancestor->rect.left;
                rect.top += ancestor->rect.top;
                rect.bottom += ancestor->rect.top;
            }
            // Generated from MinGW winuser.h MONITOR_DEFAULTTONEAREST.
            constexpr u32 nearest_monitor = 0x00000002u;
            const auto handle = m_user32_ext_mgr->monitor_from_rect_rule(rect, nearest_monitor, false);
            const auto index = m_user32_ext_mgr->monitor_index(handle);
            const auto monitors = m_user32_ext_mgr->guest_monitors();
            if (index >= 0 && static_cast<usize>(index) < monitors.size())
                mon = monitors[static_cast<usize>(index)];
        }
        const i32 w = data.rect.width(), h = data.rect.height();
        const i32 x = mon.work_left + (mon.work_right - mon.work_left - w) / 2;
        const i32 y = mon.work_top + (mon.work_bottom - mon.work_top - h) / 2;
        data.rect = {x, y, x + w, y + h};
        data.client_rect = {0, 0, w, h};
    }

    // Look up "#32770" class for the atom / GetClassName
    data.class_name = L"#32770";
    const WindowClassData* cls = m_class_mgr.find_by_name(L"#32770");
    if (cls) {
        data.class_atom = cls->atom;
        data.class_id = cls->id;
        data.class_style = cls->style;
        // DLGWINDOWEXTRA: the dialog class's cbWndExtra (measured 30 on x86
        // and x64, s1win_class_probe dialog_cbwndextra). DWLP_USER lives in it.
        data.wnd_extra_size = static_cast<u32>(std::max(0, cls->wnd_extra));
        data.extra_bytes.assign(data.wnd_extra_size, 0);
        if (!cls->class_name.empty()) {
            data.class_name = cls->class_name;
        }
    }

    // Allocate HWND
    windowing::HWND hwnd = m_hwnd_mgr.create(data);
    if (have_template) {
        i32 bx = 8, by = 16;
        if ((tmpl.style & windowing::DS::SETFONT) && tmpl.font_size > 0) {
            windowing::compute_dlu_bases(tmpl.font_size, bx, by);
        }
        m_dialog_base_units[hwnd] = {bx, by};
    }

    // WH_CBT hooks receive HCBT_CREATEWND after an HWND has been allocated
    // and before the window procedure sees creation messages. MFC relies on
    // this callback to attach its pending CWnd object to the new HWND before
    // WM_INITDIALOG is dispatched.
    if (m_creation_hook && !m_creation_hook(hwnd, data.style)) {
        VITREO_TRACE(
            "[dialog] HCBT_CREATEWND rejected dialog hwnd=0x%x\n", hwnd);
        (void)m_hwnd_mgr.destroy(hwnd);
        return windowing::NULL_HWND;
    }

    // Keep the most recently created dialog as a fallback target for APIs
    // that pass NULL/desktop HWNDs while initializing controls.
    m_current_modal_hwnd = hwnd;

    // Create native backing.
    // A WS_CHILD dialog with a real parent is embedded as a container view
    // inside that parent.
    bool is_child_dialog = (data.style & windowing::WS::CHILD) != 0 && parent != windowing::HWND_DESKTOP;
    if (is_child_dialog) {
        // Embed the child page in the inner panel control (typically id 1018)
        // when that control exists.
        windowing::HWND native_parent = parent;
        bool embed_full_parent = false;
        // Template id 1 is a full-page dialog (welcome/finish). Those pages
        // embed directly in the main dialog client.
        if (template_id == 1) {
            embed_full_parent = true;
        }

        // If parent is the main dialog, look for inner panel control (id=1018).
        // Use it for regular pages when visible; otherwise keep full-parent mode.
        DlgItemKey panel_key{parent, 1018};
        auto panel_it = m_dlg_items.find(panel_key);
        if (panel_it != m_dlg_items.end() && !embed_full_parent) {
            const windowing::WindowData* panel_data = m_hwnd_mgr.find(panel_it->second);
            bool panel_visible = panel_data && panel_data->visible;
            if (panel_visible) {
                native_parent = panel_it->second;
                VITREO_TRACE("[dialog] create_dialog: using inner panel control 0x%x as native parent\n",
                             native_parent);
            } else {
                embed_full_parent = true;
            }
        }

        // Generic panel hosts display one successive page at a time. A tab
        // control is different: every child dialog belongs to a distinct
        // native tab item, and NSTabView owns which item is visible.
        const bool is_tab_page_host =
            m_control_host.tab_get_item_count(native_parent) >= 0;

        // Hide any previous page in a generic selected host.
        windowing::HWND page_host = (native_parent != parent) ? native_parent : parent;
        if (!is_tab_page_host) {
            auto prev_it = m_panel_page.find(page_host);
            if (prev_it != m_panel_page.end() &&
                prev_it->second != windowing::NULL_HWND) {
                m_control_host.set_visible(prev_it->second, false);
            }
        }

        // Resize embedded page to host client dimensions.
        windowing::HWND size_host = embed_full_parent ? parent : native_parent;
        if (const windowing::WindowData* p = m_hwnd_mgr.find(size_host)) {
            i32 w = p->client_rect.right - p->client_rect.left;
            i32 h = p->client_rect.bottom - p->client_rect.top;
            if (w > 0 && h > 0) {
                data.rect = windowing::Rect{0, 0, w, h};
                data.client_rect = data.rect;
                if (auto* stored = m_hwnd_mgr.find(hwnd)) {
                    stored->rect = data.rect;
                    stored->client_rect = data.client_rect;
                }
                VITREO_TRACE("[dialog] create_dialog: resizing embedded child dialog 0x%x to host client %dx%d\n",
                             hwnd, w, h);
            }
        }

        // Hide placeholder overlays on full-page host to avoid blank masking.
        if (size_host == parent) {
            auto hide_host_placeholder = [&](u16 ctrl_id) {
                DlgItemKey key{parent, ctrl_id};
                auto it = m_dlg_items.find(key);
                if (it != m_dlg_items.end() && it->second != windowing::NULL_HWND) {
                    m_control_host.set_visible(it->second, false);
                }
            };
            hide_host_placeholder(1018);
            hide_host_placeholder(1044);
        }
        
        m_control_host.add_container_view(native_parent, hwnd, data);

        // Generic panel hosts track one current page. Tab controls retain all
        // pages and select them through their item views.
        if (!is_tab_page_host) {
            if (native_parent != parent) {
                m_panel_page[native_parent] = hwnd;
            } else {
                // Full-page dialog embedded in the main dialog (Welcome/Finish)
                m_panel_page[parent] = hwnd;
            }
        }
    } else {
        (void)m_control_host.create_window(hwnd, data);
    }

    // ---- Create child controls from the parsed template ----
    if (have_template && !tmpl.items.empty()) {
        create_controls_from_template(
            hwnd, tmpl, rsrc_image_base, rsrc_rva, rsrc_size);
    }

    // Learn CreateDialogParamW / DialogBoxParamW: send WM_INITDIALOG after
    // creating children and before the first display so SetWindowPos / Hide /
    // EndDialog in INITDIALOG apply to a still-hidden window.
    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createdialogparamw
    if (m_dlgproc_callback && dlgproc != 0) {
        m_dlgproc_callback(hwnd, WM_INITDIALOG,
                           static_cast<u64>(hwnd), init_param, dlgproc);
    }
    (void)post_init;

    if (m_hwnd_mgr.is_valid(hwnd) && !m_modal_ended) {
        if (windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
            win != nullptr && !win->visible && (modal || template_visible)) {
            win->visible = true;
            win->style |= windowing::WS::VISIBLE;
            m_control_host.set_visible(hwnd, true);
        }
    }

    // Diagnostic: log child page creation for debugging blank-page issues
    if (is_child_dialog && diag_logs_enabled()) {
        std::fprintf(stderr,
            "[page-diag] CreateDialogParamW: hwnd=0x%x template=%llu parent=0x%x "
            "dlgproc=0x%llx client=%dx%d\n",
            hwnd,
            static_cast<unsigned long long>(template_id),
            parent,
            static_cast<unsigned long long>(dlgproc),
            data.client_rect.right - data.client_rect.left,
            data.client_rect.bottom - data.client_rect.top);
    }

    return hwnd;
}

} // namespace vitreo::winapi
