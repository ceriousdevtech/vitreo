// SPDX-License-Identifier: LGPL-2.1-only

/// @file send_message_thunks.cpp
/// @brief SendMessageW/SendMessageA thunks and the marshaled-control entry point.

#include "winapi/user32/user32_internal.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "winapi/user32/messages/message_send_internal.hpp"
#include "winapi/user32/messages/ansi_char_message.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/model/diag_flags.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "winapi/user32/messages/cross_process_send.hpp"
#include "ipc/hwnd_registry.hpp"
#include "ipc/process_registry.hpp"
#include <unistd.h>
#include <algorithm>
#include <pthread.h>
#include <cstdlib>
#include <optional>
#include <string>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

// EN_CHANGE notification code for edit controls
constexpr u32 EN_CHANGE = 0x0300;
constexpr u16 EDIT_CLASS_ORDINAL = 0x0081;

static void send_en_change_if_edit_control(MessageSendManager& msm,
                                           windowing::HWND hwnd) {
    auto* win_data = msm.window_data(hwnd);
    if (!win_data || (win_data->class_atom != EDIT_CLASS_ORDINAL && win_data->native_control_class != L"Edit")) {
        return;
    }

    windowing::HWND parent = win_data->parent;
    u32 ctrl_id = win_data->ctrl_id;
    if (parent == windowing::HWND_DESKTOP ||
        parent == windowing::NULL_HWND || ctrl_id == 0) {
        return;
    }

    u64 cmd_wp = (static_cast<u64>(EN_CHANGE) << 16) | ctrl_id;
    VITREO_TRACE("[send] EN_CHANGE ctrl_id=%u -> parent=0x%x\n",
                 ctrl_id, parent);
    msm.send_message(parent, windowing::WM::COMMAND,
                     cmd_wp, static_cast<u64>(hwnd));
}

// ListView messages (LVM_FIRST = 0x1000)
constexpr u32 LVM_GETITEMCOUNT          = 0x1004;
constexpr u32 LVM_ENSUREVISIBLE         = 0x1013;
constexpr u32 LVM_SETCOLUMNWIDTH        = 0x101E;
constexpr u32 LVM_SETEXTENDEDLISTVIEWSTYLE = 0x1036;
constexpr u32 LVM_SETITEMW              = 0x104C;
constexpr u32 LVM_INSERTITEMW           = 0x104D;
constexpr u32 LVM_SETITEMTEXTW          = 0x1074;
constexpr u32 LVM_FIRST                 = 0x1000;
constexpr u32 LVM_LAST                  = 0x1FFF;

// LVITEMW struct offsets (32-bit)
constexpr u32 LVITEM_OFF_ITEM       = 4;
// constexpr u32 LVITEM_OFF_SUBITEM    = 8;
constexpr u32 LVITEM_OFF_PSZTEXT    = 20;

/// The window's control class is SysListView32 (case-insensitive).
static bool is_listview_window(MessageSendManager& msm, windowing::HWND hwnd) {
    const windowing::WindowData* win = msm.window_data(hwnd);
    if (win == nullptr) return false;
    const std::wstring& c = win->control_class_name();
    static constexpr std::wstring_view kListView = L"syslistview32";
    if (c.size() != kListView.size()) return false;
    for (usize i = 0; i < c.size(); ++i) {
        wchar_t ch = c[i];
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
        if (ch != kListView[i]) return false;
    }
    return true;
}

void impl_SendMessageW(Marshaler& m, MessageSendManager& msm) {
    if (send_message_to_peer_window(m, msm, /*ansi=*/false)) return;
    auto hwnd   = static_cast<windowing::HWND>(m.arg(0));
    auto msg    = static_cast<u32>(m.arg(1));
    auto wParam = m.arg(2);
    auto lParam = m.arg(3);

    // Class procedures that own their control's state come first.
    if (msm.dispatch_button_control(m, hwnd, msg, wParam, lParam, /*force_native=*/false)) {
        return;
    }
    if (msm.dispatch_syslink_control(m, hwnd, msg, wParam, lParam,
                                     /*ansi=*/false, /*force_native=*/false)) {
        return;
    }
    if (msm.dispatch_edit_control(m, hwnd, msg, wParam, lParam,
                                  /*ansi=*/false, /*force_native=*/false)) {
        return;
    }
    if (msm.dispatch_rebar_control(m, hwnd, msg, wParam, lParam)) return;
    if (try_handle_toolbar(m, msm, hwnd, msg, wParam, lParam)) return;
    if (try_handle_statusbar(m, msm, hwnd, msg, wParam, lParam)) return;
    if (msm.dispatch_tooltip_control(m, hwnd, msg, wParam, lParam)) return;

    if (try_handle_richedit_settextex(m, msm, hwnd, msg, wParam, lParam)) {
        return;
    }
    if (try_handle_richedit_streamin(m, msm, hwnd, msg, wParam, lParam)) {
        return;
    }
    if (try_handle_edit_setsel(m, msm, hwnd, msg, wParam, lParam)) {
        return;
    }
    if (try_handle_richedit_scrollpos(m, msm, hwnd, msg, wParam, lParam)) {
        return;
    }

    if (msm.dispatch_progress_range(m, hwnd, msg, wParam, lParam,
                                    /*force_native=*/false)) {
        return;
    }
    if (msm.dispatch_tab_control(
            m, hwnd, msg, wParam, lParam, /*ansi=*/false)) {
        return;
    }

    if (windowing::diag::win_diag())
        fprintf(stderr, "[snd-W] hwnd=0x%x msg=0x%x wParam=0x%llx lParam=0x%llx\n",
                hwnd, msg, (unsigned long long)wParam, (unsigned long long)lParam);

    // ── WM_GETTEXT on controls without wndproc ──────────────────────────
    // Read the Cocoa control's text and write UTF-16 to the guest buffer.
    if (msg == windowing::WM::GETTEXT && msm.wndproc_for(hwnd) == 0 && lParam != 0) {
        u32 max_chars = static_cast<u32>(wParam);
        if (max_chars == 0) { m.set_return(0u); return; }
        std::wstring text = msm.get_control_text(hwnd);
        if (text.size() > static_cast<size_t>(max_chars - 1))
            text.resize(max_chars - 1);
        std::string utf8 = wstring_to_utf8(text);
        VITREO_TRACE( "[send] WM_GETTEXT hwnd=0x%x max=%u -> \"%s\" (%zu chars)\n",
                     hwnd, max_chars, utf8.c_str(), text.size());
        (void)m.write_wide_string(static_cast<GuestAddr>(lParam), utf8);
        m.set_return(static_cast<u64>(text.size()));
        return;
    }

    // ── WM_GETTEXTLENGTH on controls without wndproc ────────────────────
    if (msg == windowing::WM::GETTEXTLENGTH && msm.wndproc_for(hwnd) == 0) {
        std::wstring text = msm.get_control_text(hwnd);
        m.set_return(static_cast<u64>(text.size()));
        return;
    }

    // ── WM_SETTEXT on controls without wndproc ──────────────────────────
    // Update the Cocoa UI, then send EN_CHANGE to the parent so dialog
    // procedures that track the edit control's text stay in sync.
    if (msg == windowing::WM::SETTEXT && msm.wndproc_for(hwnd) == 0 && lParam != 0) {
        auto str_res = m.read_wide_string(static_cast<GuestAddr>(lParam));
        if (str_res.is_ok()) {
            std::wstring ws = message_send_utf8_to_wstring(str_res.value());
            if (auto* win = msm.window_data(hwnd)) {
                win->title = ws;
            }
            (void)msm.set_control_text(hwnd, ws);

            // Mirror Windows behavior: WM_SETTEXT notifies parent only for EDIT controls.
            send_en_change_if_edit_control(msm, hwnd);

            m.set_return(1u);
            return;
        }
        m.set_return(0u);
        return;
    }

    // ── ListView (SysListView32) messages ───────────────────────────────
    // These need guest memory access to read LVITEMW structs, so they're
    // handled here at the thunk level where we have the Marshaler.
    if (msg == LVM_GETITEMCOUNT && msm.wndproc_for(hwnd) == 0) {
        i32 count = msm.bridge().listview_get_item_count(hwnd);
        VITREO_TRACE( "[send] LVM_GETITEMCOUNT hwnd=0x%x -> %d\n", hwnd, count);
        m.set_return(static_cast<u64>(count));
        return;
    }

    if (msg == LVM_INSERTITEMW && msm.wndproc_for(hwnd) == 0 && lParam != 0) {
        // Read LVITEMW.pszText pointer from guest memory
        auto psz_res = m.read_u32(static_cast<GuestAddr>(lParam + LVITEM_OFF_PSZTEXT));
        if (psz_res.is_ok() && psz_res.value() != 0) {
            auto str_res = m.read_wide_string(static_cast<GuestAddr>(psz_res.value()));
            if (str_res.is_ok()) {
                std::wstring ws = message_send_utf8_to_wstring(str_res.value());
                i32 idx = msm.bridge().listview_insert_item(hwnd, ws);
                m.set_return(static_cast<u64>(idx >= 0 ? idx : -1));
                return;
            }
        }
        // Fallback: insert empty line
        i32 idx = msm.bridge().listview_insert_item(hwnd, L"");
        m.set_return(static_cast<u64>(idx >= 0 ? idx : -1));
        return;
    }

    if ((msg == LVM_SETITEMTEXTW || msg == LVM_SETITEMW) && msm.wndproc_for(hwnd) == 0 && lParam != 0) {
        // For SETITEMTEXTW: wParam = item index
        // For SETITEMW: item index is in LVITEMW.iItem
        i32 item_index;
        if (msg == LVM_SETITEMTEXTW) {
            item_index = static_cast<i32>(wParam);
        } else {
            auto idx_res = m.read_u32(static_cast<GuestAddr>(lParam + LVITEM_OFF_ITEM));
            item_index = idx_res.is_ok() ? static_cast<i32>(idx_res.value()) : 0;
        }
        auto psz_res = m.read_u32(static_cast<GuestAddr>(lParam + LVITEM_OFF_PSZTEXT));
        if (psz_res.is_ok() && psz_res.value() != 0) {
            auto str_res = m.read_wide_string(static_cast<GuestAddr>(psz_res.value()));
            if (str_res.is_ok()) {
                std::wstring ws = message_send_utf8_to_wstring(str_res.value());
                msm.bridge().listview_set_item_text(hwnd, item_index, ws);
            }
        }
        m.set_return(1u); // TRUE
        return;
    }

    if (msg == LVM_ENSUREVISIBLE && msm.wndproc_for(hwnd) == 0) {
        i32 item_index = static_cast<i32>(wParam);
        msm.bridge().listview_ensure_visible(hwnd, item_index);
        m.set_return(1u); // TRUE
        return;
    }

    if ((msg == LVM_SETEXTENDEDLISTVIEWSTYLE || msg == LVM_SETCOLUMNWIDTH)
         && msm.wndproc_for(hwnd) == 0) {
        // Acknowledge style/column width changes silently
        m.set_return(0u);
        return;
    }

    // Catch-all: silently acknowledge any other LVM_* messages for list-view
    // controls without a wndproc. The 0x1000-0x1FFF range is shared by every
    // common control (HDM 0x1200, TCM 0x1300, ECM 0x1500, BCM 0x1600,
    // CBM 0x1700), so only a SysListView32 may take it as LVM_*.
    if (msg >= LVM_FIRST && msg <= LVM_LAST && msm.wndproc_for(hwnd) == 0 &&
        is_listview_window(msm, hwnd)) {
        VITREO_TRACE( "[send] LVM msg=0x%x hwnd=0x%x (unhandled, ack)\n", msg, hwnd);
        m.set_return(0u);
        return;
    }

    // ── TreeView (SysTreeView32) messages (shared A/W handler) ───────────
    if (try_handle_treeview(m, msm, hwnd, msg, wParam, lParam)) return;

    // ── ComboBox/ListBox messages (shared handler; W = wide strings) ─────
    if (try_handle_combolist(m, msm, hwnd, msg, wParam, lParam, /*ansi=*/false)) return;

    // Always-on: trace SendMessage for notification/command messages to dialog HWNDs
    if (message_send_diag_logs_enabled() && (msg == 0x004E || msg == 0x0111 || msg == 0x0010 || (msg >= 0x0400 && msg <= 0x04FF))) {
        std::fprintf(stderr,
            "[send-diag] SendMessageW hwnd=0x%x msg=0x%x wParam=0x%llx lParam=0x%llx wndproc=0x%llx\n",
            hwnd, msg,
            static_cast<unsigned long long>(wParam),
            static_cast<unsigned long long>(lParam),
            static_cast<unsigned long long>(msm.wndproc_for(hwnd)));
    }
    u64 result = msm.send_message(hwnd, msg, wParam, lParam);
    // Always-on: trace SendMessage return for notification messages
    if (message_send_diag_logs_enabled() && (msg == 0x004E || msg == 0x0111 || (msg >= 0x0400 && msg <= 0x04FF))) {
        std::fprintf(stderr,
            "[send-diag] SendMessageW result: hwnd=0x%x msg=0x%x -> 0x%llx\n",
            hwnd, msg, static_cast<unsigned long long>(result));
    }
    m.set_return(static_cast<u64>(result));
}

bool MessageSendManager::dispatch_marshaled_control(Marshaler& m, windowing::HWND hwnd,
                                                    u32 msg, u64 wParam, u64 lParam, bool ansi,
                                                    bool force_native) {
    if (dispatch_button_control(m, hwnd, msg, wParam, lParam, force_native)) return true;
    if (dispatch_syslink_control(m, hwnd, msg, wParam, lParam, ansi, force_native)) return true;
    if (dispatch_edit_control(m, hwnd, msg, wParam, lParam, ansi, force_native)) return true;
    if (dispatch_progress_range(m, hwnd, msg, wParam, lParam, force_native)) return true;
    if (dispatch_rebar_control(m, hwnd, msg, wParam, lParam)) return true;
    if (try_handle_toolbar(m, *this, hwnd, msg, wParam, lParam)) return true;
    if (try_handle_statusbar(m, *this, hwnd, msg, wParam, lParam)) return true;
    if (dispatch_tooltip_control(m, hwnd, msg, wParam, lParam)) return true;
    if (try_handle_richedit_settextex(m, *this, hwnd, msg, wParam, lParam)) {
        return true;
    }
    if (try_handle_richedit_streamin(m, *this, hwnd, msg, wParam, lParam,
                                     force_native)) {
        return true;
    }
    if (try_handle_edit_setsel(m, *this, hwnd, msg, wParam, lParam)) {
        return true;
    }
    if (try_handle_richedit_scrollpos(m, *this, hwnd, msg, wParam, lParam)) {
        return true;
    }
    // ComboBox/ListBox population + queries (need the Marshaler for guest strings).
    if (try_handle_combolist(m, *this, hwnd, msg, wParam, lParam, ansi, force_native)) {
        return true;
    }
    // Button-check, static-image, and progress-bar messages (no guest
    // strings). SendDlgItemMessage relies on this even when a control is
    // lightly subclassed (BM_GETCHECK included).
    u64 result = 0;
    if (handle_control_message(hwnd, msg, wParam, lParam, result)) {
        m.set_return(result);
        return true;
    }
    return false;
}

void impl_SendMessageA(Marshaler& m, MessageSendManager& msm) {
    if (send_message_to_peer_window(m, msm, /*ansi=*/true)) return;
    auto hwnd   = static_cast<windowing::HWND>(m.arg(0));
    auto msg    = static_cast<u32>(m.arg(1));
    auto wParam = m.arg(2);
    auto lParam = m.arg(3);

    // Class procedures that own their control's state come first.
    if (msm.dispatch_button_control(m, hwnd, msg, wParam, lParam, /*force_native=*/false)) {
        return;
    }
    if (msm.dispatch_syslink_control(m, hwnd, msg, wParam, lParam,
                                     /*ansi=*/true, /*force_native=*/false)) {
        return;
    }
    if (msm.dispatch_edit_control(m, hwnd, msg, wParam, lParam,
                                  /*ansi=*/true, /*force_native=*/false)) {
        return;
    }
    if (msm.dispatch_rebar_control(m, hwnd, msg, wParam, lParam)) return;
    if (try_handle_toolbar(m, msm, hwnd, msg, wParam, lParam)) return;
    if (try_handle_statusbar(m, msm, hwnd, msg, wParam, lParam)) return;

    if (try_handle_richedit_settextex(m, msm, hwnd, msg, wParam, lParam)) {
        return;
    }
    if (try_handle_richedit_streamin(m, msm, hwnd, msg, wParam, lParam)) {
        return;
    }
    if (try_handle_edit_setsel(m, msm, hwnd, msg, wParam, lParam)) {
        return;
    }
    if (try_handle_richedit_scrollpos(m, msm, hwnd, msg, wParam, lParam)) {
        return;
    }

    if (msm.dispatch_progress_range(m, hwnd, msg, wParam, lParam,
                                    /*force_native=*/false)) {
        return;
    }
    if (msm.dispatch_tab_control(
            m, hwnd, msg, wParam, lParam, /*ansi=*/true)) {
        return;
    }

    if (windowing::diag::win_diag())
        fprintf(stderr, "[snd-A] hwnd=0x%x msg=0x%x wParam=0x%llx lParam=0x%llx\n",
                hwnd, msg, (unsigned long long)wParam, (unsigned long long)lParam);

    // ── TreeView (SysTreeView32) messages — PuTTY (ANSI) populates its
    //    category tree via SendMessageA(TVM_INSERTITEMA), so this path must
    //    handle them just like the W thunk does.
    if (try_handle_treeview(m, msm, hwnd, msg, wParam, lParam)) return;

    // ── ComboBox/ListBox messages (shared handler; A = ANSI strings) ─────
    if (try_handle_combolist(m, msm, hwnd, msg, wParam, lParam, /*ansi=*/true)) return;

    // ── WM_GETTEXT (ANSI) on controls without wndproc ───────────────────
    if (msg == windowing::WM::GETTEXT && msm.wndproc_for(hwnd) == 0 && lParam != 0) {
        u32 max_chars = static_cast<u32>(wParam);
        if (max_chars == 0) { m.set_return(0u); return; }
        std::wstring text = msm.get_control_text(hwnd);
        std::string utf8 = wstring_to_utf8(text);
        if (utf8.size() > static_cast<size_t>(max_chars - 1))
            utf8.resize(max_chars - 1);
        VITREO_TRACE( "[send] WM_GETTEXT(A) hwnd=0x%x max=%u -> \"%s\"\n",
                     hwnd, max_chars, utf8.c_str());
        (void)m.write_ansi_string(static_cast<GuestAddr>(lParam), utf8);
        m.set_return(static_cast<u64>(utf8.size()));
        return;
    }

    // ── WM_GETTEXTLENGTH (ANSI) on controls without wndproc ─────────────
    if (msg == windowing::WM::GETTEXTLENGTH && msm.wndproc_for(hwnd) == 0) {
        std::wstring text = msm.get_control_text(hwnd);
        std::string utf8 = wstring_to_utf8(text);
        m.set_return(static_cast<u64>(utf8.size()));
        return;
    }

    // ── WM_SETTEXT (ANSI) on controls without wndproc ───────────────────
    if (msg == windowing::WM::SETTEXT && msm.wndproc_for(hwnd) == 0 && lParam != 0) {
        auto str_res = m.read_ansi_string(static_cast<GuestAddr>(lParam));
        if (str_res.is_ok()) {
            std::wstring ws = message_send_utf8_to_wstring(str_res.value());
            if (auto* win = msm.window_data(hwnd)) {
                win->title = ws;
            }
            (void)msm.set_control_text(hwnd, ws);

            // Mirror Windows behavior: WM_SETTEXT notifies parent only for EDIT controls.
            send_en_change_if_edit_control(msm, hwnd);

            m.set_return(1u);
            return;
        }
        m.set_return(0u);
        return;
    }

    // ── ComboBox/ListBox messages (ANSI) ─────────────────────────────────
    if (msm.wndproc_for(hwnd) == 0) {
        switch (msg) {
        case CB_ADDSTRING:
        case CB_INSERTSTRING:
        case LB_ADDSTRING:
        case LB_INSERTSTRING: {
            std::wstring ws;
            if (lParam != 0) {
                auto str_res = m.read_ansi_string(static_cast<GuestAddr>(lParam));
                if (str_res.is_ok()) {
                    ws = message_send_utf8_to_wstring(str_res.value());
                }
            }
            i32 index = (msg == CB_INSERTSTRING || msg == LB_INSERTSTRING)
                ? static_cast<i32>(wParam)
                : -1;
            i32 out = (msg == LB_ADDSTRING || msg == LB_INSERTSTRING)
                ? msm.bridge().listbox_add_item(hwnd, ws, index)
                : msm.bridge().combo_add_item(hwnd, ws, index);
            m.set_return(static_cast<u64>(static_cast<i32>(out)));
            return;
        }
        case CB_RESETCONTENT:
        case LB_RESETCONTENT: {
            bool ok = (msg == LB_RESETCONTENT)
                ? msm.bridge().listbox_reset_content(hwnd)
                : msm.bridge().combo_reset_content(hwnd);
            (void)ok;
            m.set_return(0u);
            return;
        }
        case CB_GETCOUNT:
        case LB_GETCOUNT: {
            i32 count = (msg == LB_GETCOUNT)
                ? msm.bridge().listbox_get_count(hwnd)
                : msm.bridge().combo_get_count(hwnd);
            m.set_return(static_cast<u64>(static_cast<i32>(count)));
            return;
        }
        case CB_SETCURSEL:
        case LB_SETCURSEL: {
            i32 idx = static_cast<i32>(wParam);
            i32 sel = (msg == LB_SETCURSEL)
                ? msm.bridge().listbox_set_cur_sel(hwnd, idx)
                : msm.bridge().combo_set_cur_sel(hwnd, idx);
            m.set_return(static_cast<u64>(static_cast<i32>(sel)));
            return;
        }
        case CB_GETCURSEL:
        case LB_GETCURSEL: {
            i32 sel = (msg == LB_GETCURSEL)
                ? msm.bridge().listbox_get_cur_sel(hwnd)
                : msm.bridge().combo_get_cur_sel(hwnd);
            m.set_return(static_cast<u64>(static_cast<i32>(sel)));
            return;
        }
        case CB_GETLBTEXTLEN:
        case LB_GETTEXTLEN: {
            i32 idx = static_cast<i32>(wParam);
            std::wstring ws = (msg == LB_GETTEXTLEN)
                ? msm.bridge().listbox_get_item_text(hwnd, idx)
                : msm.bridge().combo_get_item_text(hwnd, idx);
            std::string utf8 = wstring_to_utf8(ws);
            i32 len = utf8.empty() ? CB_ERR : static_cast<i32>(utf8.size());
            m.set_return(static_cast<u64>(static_cast<i32>(len)));
            return;
        }
        case CB_GETLBTEXT:
        case LB_GETTEXT: {
            i32 idx = static_cast<i32>(wParam);
            std::wstring ws = (msg == LB_GETTEXT)
                ? msm.bridge().listbox_get_item_text(hwnd, idx)
                : msm.bridge().combo_get_item_text(hwnd, idx);
            if (ws.empty() || lParam == 0) {
                m.set_return(static_cast<u64>(static_cast<i32>(CB_ERR)));
                return;
            }
            std::string utf8 = wstring_to_utf8(ws);
            (void)m.write_ansi_string(static_cast<GuestAddr>(lParam), utf8);
            m.set_return(static_cast<u64>(static_cast<i32>(utf8.size())));
            return;
        }
        case LB_DELETESTRING: {
            i32 idx = static_cast<i32>(wParam);
            i32 remaining = msm.bridge().listbox_delete_item(hwnd, idx);
            m.set_return(static_cast<u64>(static_cast<i32>(remaining)));
            return;
        }
        default:
            break;
        }
    }

    // For all other messages, delegate to the W send_message path
    // Always-on: trace SendMessage for notification/command messages
    if (message_send_diag_logs_enabled() && (msg == 0x004E || msg == 0x0111 || msg == 0x0010 || (msg >= 0x0400 && msg <= 0x04FF))) {
        std::fprintf(stderr,
            "[send-diag] SendMessageA hwnd=0x%x msg=0x%x wParam=0x%llx lParam=0x%llx wndproc=0x%llx\n",
            hwnd, msg,
            static_cast<unsigned long long>(wParam),
            static_cast<unsigned long long>(lParam),
            static_cast<unsigned long long>(msm.wndproc_for(hwnd)));
    }
    u64 result = msm.send_message(hwnd, msg, wParam, lParam);
    if (message_send_diag_logs_enabled() && (msg == 0x004E || msg == 0x0111 || (msg >= 0x0400 && msg <= 0x04FF))) {
        std::fprintf(stderr,
            "[send-diag] SendMessageA result: hwnd=0x%x msg=0x%x -> 0x%llx\n",
            hwnd, msg, static_cast<unsigned long long>(result));
    }
    m.set_return(static_cast<u64>(result));
}

} // namespace vitreo::winapi
