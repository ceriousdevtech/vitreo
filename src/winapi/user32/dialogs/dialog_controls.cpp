// SPDX-License-Identifier: LGPL-2.1-only

/// @file dialog_controls.cpp
/// @brief Dialog button and SendDlgItemMessage helpers.
///
/// Closed cut from user32_ext.cpp — verified with plan_extraction.py.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/resources/dialog_template.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

// ---------- Dialog / button ----------

void User32ExtManager::impl_CheckDlgButton(Marshaler& m) {
    // BOOL CheckDlgButton(HWND, int nIDButton, UINT uCheck): BM_SETCHECK to
    // the control (Learn); a missing control is ERROR_CONTROL_ID_NOT_FOUND
    // (measured, checkdlgbutton_missing).
    constexpr u32 BM_SETCHECK = 0x00F1;
    const auto dlg = static_cast<windowing::HWND>(m.arg(0));
    const auto id = static_cast<u32>(m.arg(1));
    const auto check = static_cast<u32>(m.arg(2));
    const windowing::HWND ctrl = m_dialog_mgr ? m_dialog_mgr->get_dlg_item(dlg, id) : 0;
    if (ctrl == windowing::NULL_HWND) {
        set_last_error(1421);
        m.set_return(0u);
        return;
    }
    if (m_msgsend_mgr) (void)m_msgsend_mgr->send_message(ctrl, BM_SETCHECK, check, 0);
    else (void)m_host.set_button_check(ctrl, check != 0);
    m.set_return(1u);
}

void User32ExtManager::impl_IsDlgButtonChecked(Marshaler& m) {
    // UINT IsDlgButtonChecked(HWND, int): BM_GETCHECK to the control.
    constexpr u32 BM_GETCHECK = 0x00F0;
    const auto dlg = static_cast<windowing::HWND>(m.arg(0));
    const auto id = static_cast<u32>(m.arg(1));
    const windowing::HWND ctrl = m_dialog_mgr ? m_dialog_mgr->get_dlg_item(dlg, id) : 0;
    if (ctrl == windowing::NULL_HWND) {
        set_last_error(1421);
        m.set_return(0u);
        return;
    }
    const u64 state = m_msgsend_mgr ? m_msgsend_mgr->send_message(ctrl, BM_GETCHECK, 0, 0)
                                    : m_host.get_button_check(ctrl);
    m.set_return(static_cast<u32>(state));
}

void User32ExtManager::impl_SendDlgItemMessageW(Marshaler& m) {
    send_dlg_item_message_impl(m, /*ansi=*/false);
}

void User32ExtManager::send_dlg_item_message_impl(Marshaler& m, bool ansi) {
    auto hDlg   = static_cast<windowing::HWND>(m.arg(0));
    auto nID    = static_cast<u32>(m.arg(1));
    auto msg    = static_cast<u32>(m.arg(2));
    auto wParam = m.arg(3);
    auto lParam = m.arg(4);

    if (hDlg == windowing::NULL_HWND) {
        if (m_active_window != 0 && m_hwnd_mgr.is_valid(static_cast<windowing::HWND>(m_active_window))) {
            hDlg = static_cast<windowing::HWND>(m_active_window);
        } else {
            auto top = m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
            if (!top.empty()) {
                hDlg = top.back();
            }
        }
    }

    VITREO_TRACE( "[user32_ext] SendDlgItemMessageW(dlg=0x%x, id=%u, msg=0x%x)\n",
                 hDlg, nID, msg);
    if (std::getenv("VITREO_WIN_DIAG"))
        fprintf(stderr, "[dlgmsg] SendDlgItemMessage dlg=0x%x id=%u msg=0x%x wParam=0x%llx\n",
                hDlg, nID, msg, (unsigned long long)wParam);

    // Find child window
    windowing::HWND child = windowing::NULL_HWND;
    auto children = m_hwnd_mgr.enum_children(hDlg);
    for (auto ch : children) {
        const auto* wd = m_hwnd_mgr.find(ch);
        if (wd && wd->ctrl_id == nID) {
            child = ch;
            break;
        }
    }

    if (child == windowing::NULL_HWND) {
        m.set_return(0u);
        return;
    }

    if (m_msgsend_mgr) {
        // Control messages that need the Marshaler (combobox/listbox population &
        // queries, button-check, etc.) — PuTTY populates dropdowns/lists via this
        // path, so route through the shared handler before the generic wndproc
        // dispatch (which can't read guest strings).
        if (m_msgsend_mgr->dispatch_marshaled_control(m, child, msg, wParam, lParam, ansi)) {
            return;
        }
        auto r = m_msgsend_mgr->send_message(child, msg, wParam, lParam);
        m.set_return(r);
        return;
    }

    // Handle BM_SETCHECK / BM_GETCHECK locally
    constexpr u32 BM_GETCHECK = 0x00F0;
    constexpr u32 BM_SETCHECK = 0x00F1;
    if (msg == BM_SETCHECK) {
        m_host.set_button_check(child, wParam != 0);
        m.set_return(0u);
    } else if (msg == BM_GETCHECK) {
        // We don't have direct access to MessageSendManager state here,
        // so return 0 (unchecked) as default
        m.set_return(0u);
    } else {
        // Other messages: return 0 as stub
        m.set_return(0u);
    }
}

void User32ExtManager::impl_SendDlgItemMessageA(Marshaler& m) {
    // ANSI: combobox/listbox string payloads (CB_ADDSTRING, etc.) are ANSI.
    send_dlg_item_message_impl(m, /*ansi=*/true);
}

} // namespace vitreo::winapi
