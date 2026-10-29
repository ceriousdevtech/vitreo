// SPDX-License-Identifier: LGPL-2.1-only

/// @file control_combolist.cpp
/// @brief ComboBox/ListBox population and queries shared by SendMessage{A,W} and
///        SendDlgItemMessage.

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

// ── ComboBox / ListBox messages (shared A/W handler) ────────────────────────
// Population + selection for native combobox/listbox controls. Lives here (not
// duplicated inline) so SendMessage{A,W} AND SendDlgItemMessage{A,W} all hit it —
// PuTTY populates via SendDlgItemMessage, which previously bypassed this and left
// dropdowns/lists empty. `ansi` selects the string encoding (CB_ADDSTRING etc.).
bool try_handle_combolist(Marshaler& m, MessageSendManager& msm,
                          windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam,
                          bool ansi, bool force_native) {
    // Subclassed controls: SendMessage must reach the guest proc first (it will
    // CallWindowProc the Def*Proc). CallWindowProc's native sentinel path sets
    // force_native so LB_ADDSTRING still fills the Cocoa listbox.
    if (msm.dispatch_owner_draw_list(m, hwnd, msg, wParam, lParam, force_native, ansi)) return true;
    if (!force_native && msm.wndproc_for(hwnd) != 0) return false;
    switch (msg) {
    case CB_ADDSTRING:
    case CB_INSERTSTRING:
    case LB_ADDSTRING:
    case LB_INSERTSTRING: {
        std::wstring ws;
        if (lParam != 0) {
            auto str_res = ansi ? m.read_ansi_string(static_cast<GuestAddr>(lParam))
                                : m.read_wide_string(static_cast<GuestAddr>(lParam));
            if (str_res.is_ok()) ws = message_send_utf8_to_wstring(str_res.value());
        }
        i32 index = (msg == CB_INSERTSTRING || msg == LB_INSERTSTRING)
                    ? static_cast<i32>(wParam) : -1;
        i32 out = (msg == LB_ADDSTRING || msg == LB_INSERTSTRING)
            ? msm.bridge().listbox_add_item(hwnd, ws, index)
            : msm.bridge().combo_add_item(hwnd, ws, index);
        m.set_return(static_cast<u64>(static_cast<i32>(out)));
        return true;
    }
    case CB_RESETCONTENT:
    case LB_RESETCONTENT:
        if (msg == LB_RESETCONTENT) (void)msm.bridge().listbox_reset_content(hwnd);
        else                        (void)msm.bridge().combo_reset_content(hwnd);
        m.set_return(0u);
        return true;
    case CB_GETCOUNT:
    case LB_GETCOUNT: {
        i32 count = (msg == LB_GETCOUNT) ? msm.bridge().listbox_get_count(hwnd)
                                         : msm.bridge().combo_get_count(hwnd);
        m.set_return(static_cast<u64>(static_cast<i32>(count)));
        return true;
    }
    case CB_SETCURSEL:
    case LB_SETCURSEL: {
        i32 idx = static_cast<i32>(wParam);
        i32 sel = (msg == LB_SETCURSEL) ? msm.bridge().listbox_set_cur_sel(hwnd, idx)
                                        : msm.bridge().combo_set_cur_sel(hwnd, idx);
        m.set_return(static_cast<u64>(static_cast<i32>(sel)));
        return true;
    }
    case CB_GETCURSEL:
    case LB_GETCURSEL: {
        i32 sel = (msg == LB_GETCURSEL) ? msm.bridge().listbox_get_cur_sel(hwnd)
                                        : msm.bridge().combo_get_cur_sel(hwnd);
        m.set_return(static_cast<u64>(static_cast<i32>(sel)));
        return true;
    }
    case CB_GETLBTEXTLEN:
    case LB_GETTEXTLEN: {
        i32 idx = static_cast<i32>(wParam);
        std::wstring ws = (msg == LB_GETTEXTLEN) ? msm.bridge().listbox_get_item_text(hwnd, idx)
                                                 : msm.bridge().combo_get_item_text(hwnd, idx);
        i32 len = ws.empty() ? CB_ERR : static_cast<i32>(ws.size());
        m.set_return(static_cast<u64>(static_cast<i32>(len)));
        return true;
    }
    case CB_GETLBTEXT:
    case LB_GETTEXT: {
        i32 idx = static_cast<i32>(wParam);
        std::wstring ws = (msg == LB_GETTEXT) ? msm.bridge().listbox_get_item_text(hwnd, idx)
                                              : msm.bridge().combo_get_item_text(hwnd, idx);
        if (ws.empty() || lParam == 0) {
            m.set_return(static_cast<u64>(static_cast<i32>(CB_ERR)));
            return true;
        }
        std::string utf8 = wstring_to_utf8(ws);
        if (ansi) (void)m.write_ansi_string(static_cast<GuestAddr>(lParam), utf8);
        else      (void)m.write_wide_string(static_cast<GuestAddr>(lParam), utf8);
        m.set_return(static_cast<u64>(static_cast<i32>(ws.size())));
        return true;
    }
    case LB_DELETESTRING: {
        i32 idx = static_cast<i32>(wParam);
        i32 remaining = msm.bridge().listbox_delete_item(hwnd, idx);
        m.set_return(static_cast<u64>(static_cast<i32>(remaining)));
        return true;
    }
    case CB_SETITEMDATA: {
        // CB_SETITEMDATA stores the per-item value CB_GETITEMDATA returns.
        // A missing store answers 0 for every item.
        const i32 idx = static_cast<i32>(wParam);
        const bool ok = msm.bridge().combo_set_item_data(hwnd, idx, lParam);
        VITREO_TRACE("[send] CB_SETITEMDATA hwnd=0x%x idx=%d data=0x%llx -> %s\n",
                     hwnd, idx, static_cast<unsigned long long>(lParam),
                     ok ? "OK" : "ERR");
        m.set_return(ok ? static_cast<u64>(CB_OKAY)
                        : static_cast<u64>(static_cast<i32>(CB_ERR)));
        return true;
    }
    case CB_GETITEMDATA: {
        const i32 idx = static_cast<i32>(wParam);
        bool ok = false;
        const u64 data = msm.bridge().combo_get_item_data(hwnd, idx, &ok);
        VITREO_TRACE("[send] CB_GETITEMDATA hwnd=0x%x idx=%d -> 0x%llx ok=%d\n",
                     hwnd, idx, static_cast<unsigned long long>(data), ok ? 1 : 0);
        m.set_return(ok ? data : static_cast<u64>(static_cast<i32>(CB_ERR)));
        return true;
    }
    default:
        return false;
    }
}

} // namespace vitreo::winapi

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include <atomic>
#include <cwctype>

namespace vitreo::winapi {

// BOOL GetComboBoxInfo(HWND hwndCombo, PCOMBOBOXINFO pcbi). Learn gives the
// struct (winuser.h tagCOMBOBOXINFO: 52 bytes on x86, 64 on x64) and "zero
// on failure, GetLastError"; the rest is u32_char_dde_probe "-- comboboxinfo":
//  * an invalid or NULL window is ERROR_INVALID_WINDOW_HANDLE; a NULL pcbi
//    ERROR_NOACCESS;
//  * a real combo box needs cbSize == sizeof(COMBOBOXINFO), otherwise
//    ERROR_INVALID_PARAMETER;
//  * any other window is sent CB_GETCOMBOBOXINFO and its LRESULT is the
//    return value as is (a procedure returning 7 makes GetComboBoxInfo 7;
//    DefWindowProc makes it 0 with the last error untouched). On x86 the size
//    is checked first as for a combo box; on x64 it is not
//    (cbi_custom_badsize).
// Vitreo's combo box is a native control with no Edit or ComboLBox child
// window, so it has no hwndItem/hwndList to report: that case fails loudly.
void User32ExtManager::impl_GetComboBoxInfo(Marshaler& m) {
    constexpr u32 kCbGetComboBoxInfo = 0x0164;
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr pcbi = m.arg_ptr(1);
    const windowing::WindowData* win =
        hwnd != windowing::NULL_HWND ? m_hwnd_mgr.find(hwnd) : nullptr;
    if (win == nullptr) {
        if (hwnd != windowing::NULL_HWND &&
            ipc::hwnd_registry_is_window(static_cast<u32>(hwnd))) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!GetComboBoxInfo: "
                                     "window of another process\n");
            }
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        } else {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        }
        m.set_return(0u);
        return;
    }
    const auto cb_size = m.read_u32(pcbi);
    if (pcbi == 0 || cb_size.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    const u32 expected = m.cpu_is_64bit() ? 64u : 52u;
    std::wstring cls = win->control_class_name();
    for (auto& c : cls) c = static_cast<wchar_t>(std::towlower(c));
    const bool combo = cls == L"combobox";
    if ((combo || !m.cpu_is_64bit()) && cb_size.value() != expected) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    if (combo) {
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set()) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!GetComboBoxInfo: "
                                 "native combo box has no Edit/ComboLBox child "
                                 "windows for hwndItem/hwndList\n");
        }
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0u);
        return;
    }
    if (m_msgsend_mgr == nullptr) {
        m.set_return(0u);
        return;
    }
    const u64 result = m_msgsend_mgr->send_message(hwnd, kCbGetComboBoxInfo, 0, pcbi);
    m.set_return(static_cast<u32>(result));
}

} // namespace vitreo::winapi
