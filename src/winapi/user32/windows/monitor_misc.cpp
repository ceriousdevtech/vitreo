// SPDX-License-Identifier: LGPL-2.1-only

/// @file monitor_misc.cpp
/// @brief Monitor and misc window stubs.
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
#include <cerrno>
#include <csignal>

#include <unistd.h>

namespace vitreo::winapi {

// ---------- Monitor / misc stubs ----------

void User32ExtManager::impl_AllowSetForegroundWindow(Marshaler& m) {
    // BOOL AllowSetForegroundWindow(DWORD dwProcessId): ASFW_ANY or an
    // existing process; anything else fails with ERROR_INVALID_PARAMETER
    // (s2msg_input_probe allowsetfg_zero / allowsetfg_bogus_pid). Guest
    // process ids are host pids. The grant itself needs no bookkeeping:
    // Vitreo does not refuse SetForegroundWindow to any guest process.
    const u32 pid = static_cast<u32>(m.arg(0));
    constexpr u32 ASFW_ANY = 0xFFFFFFFFu;
    bool exists = pid == ASFW_ANY;
    if (!exists && pid != 0 && pid <= 0x7FFFFFFFu) {
        exists = ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
    }
    if (!exists) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    VITREO_TRACE("[user32_ext] AllowSetForegroundWindow pid=%u -> TRUE\n", pid);
    m.set_return(1); // TRUE
}

void User32ExtManager::impl_EnumChildWindows(Marshaler& m) {
    // BOOL EnumChildWindows(HWND hWndParent, WNDENUMPROC lpEnumFunc, LPARAM)
    //
    // Learn: enumerates the child windows of hWndParent, including their
    // descendants, stopping when the callback returns FALSE; a NULL parent is
    // EnumWindows. Measured (s1win_api_probe enumchildwindows_*): the order
    // is depth-first pre-order in Z order; the return value is TRUE only when
    // at least one child was enumerated and none stopped it; an invalid
    // parent is ERROR_INVALID_WINDOW_HANDLE.
    const auto parent = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr enum_proc = m.arg_ptr(1);
    const u64 lparam = m.arg(2);
    if (parent == windowing::NULL_HWND) {
        impl_EnumWindows_values(m, enum_proc, lparam);
        return;
    }
    const bool local = m_hwnd_mgr.is_valid(parent);
    if (!local && !ipc::hwnd_registry_is_window(static_cast<u32>(parent))) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (!m_guest_call || enum_proc == 0) {
        m.set_return(0u);
        return;
    }

    // Snapshot the whole list first (Windows builds it before calling back):
    // the callback may create or destroy windows.
    std::vector<ipc::HwndRegistryEntry> peers;
    const u32 self_pid = static_cast<u32>(::getpid());
    for (const auto& entry : ipc::hwnd_registry_enumerate()) {
        if (entry.owner_host_pid != self_pid) peers.push_back(entry);
    }
    std::vector<windowing::HWND> order;
    auto collect = [&](auto&& self, windowing::HWND p) -> void {
        for (windowing::HWND child : m_hwnd_mgr.enum_children(p)) {
            order.push_back(child);
            self(self, child);
        }
        // Children another capsule process created under this window.
        for (const auto& peer : peers) {
            if (peer.parent_hwnd == p && peer.hwnd != p) {
                order.push_back(static_cast<windowing::HWND>(peer.hwnd));
                self(self, static_cast<windowing::HWND>(peer.hwnd));
            }
        }
    };
    collect(collect, parent);

    const bool is_64bit = m.cpu_is_64bit();
    bool any = false;
    for (windowing::HWND child : order) {
        if (!m_hwnd_mgr.is_valid(child) &&
            !ipc::hwnd_registry_is_window(static_cast<u32>(child))) {
            continue;  // destroyed by an earlier callback
        }
        any = true;
        const u64 args[2] = {static_cast<u64>(child), lparam};
        if (m_guest_call(enum_proc, args, 2, is_64bit) == 0) {
            m.set_return(0u);
            return;
        }
    }
    m.set_return(any ? 1u : 0u);
}

void User32ExtManager::impl_PostThreadMessageW(Marshaler& m) {
    (void)m.arg(0); // idThread
    (void)m.arg(1); // Msg
    (void)m.arg(2); // wParam
    (void)m.arg(3); // lParam
    VITREO_TRACE( "[user32_ext] PostThreadMessageW stub → TRUE\n");
    m.set_return(1); // TRUE
}

void User32ExtManager::impl_RedrawWindow(Marshaler& m) {
    auto hwnd  = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr rect_ptr = m.arg_ptr(1);
    const auto hrgn = static_cast<u64>(m.arg(2));
    auto flags = static_cast<u32>(m.arg32(3));
    // Not a window: FALSE, error untouched ("RedrawWindow(bad hwnd)=0").
    if (hwnd != windowing::NULL_HWND && m_paint_mgr != nullptr && !m_paint_mgr->is_window(hwnd)) {
        m.set_return(0u);
        return;
    }
    // The rectangle (or region) scopes RDW_INVALIDATE / RDW_VALIDATE ("WM_PAINT
    // ... rc=1,2,3,4" for RedrawWindow(hwnd, {1,2,3,4}, ...)).
    Rect part{};
    const bool has_rect = rect_ptr != 0 &&
        m.memory().read_bytes(rect_ptr, reinterpret_cast<u8*>(&part), sizeof(part)).is_ok();

    // RDW flag constants
    constexpr u32 RDW_INVALIDATE  = 0x0001;
    constexpr u32 RDW_ERASE       = 0x0004;
    constexpr u32 RDW_VALIDATE    = 0x0008;
    constexpr u32 RDW_UPDATENOW   = 0x0100;
    constexpr u32 RDW_ERASENOW    = 0x0200;

    VITREO_TRACE("[user32_ext] RedrawWindow hwnd=0x%x flags=0x%x\n", hwnd, flags);

    if (flags & RDW_VALIDATE) {
        if (m_paint_mgr) {
            if (hrgn != 0) (void)m_paint_mgr->validate_rgn(static_cast<u64>(hwnd), hrgn);
            else (void)m_paint_mgr->validate_rect(static_cast<u64>(hwnd), has_rect ? &part : nullptr);
        }
    }

    if (flags & RDW_INVALIDATE) {
        if (m_paint_mgr) {
            bool erase = (flags & (RDW_ERASE | RDW_ERASENOW)) != 0;
            if (hrgn != 0) (void)m_paint_mgr->invalidate_rgn(static_cast<u64>(hwnd), hrgn, erase);
            else (void)m_paint_mgr->invalidate_rect(static_cast<u64>(hwnd), has_rect ? &part : nullptr, erase);
        }
    }

    if (flags & RDW_UPDATENOW) {
        if (m_paint_mgr && m_msgloop_mgr &&
            m_paint_mgr->needs_paint(static_cast<u64>(hwnd))) {
            windowing::MSG paint_msg{};
            paint_msg.hwnd    = hwnd;
            paint_msg.message = windowing::WM::PAINT;
            m_msgloop_mgr->dispatch_message(paint_msg);
        }
    }

    m.set_return(1); // TRUE
}

void User32ExtManager::impl_SetDlgItemInt(Marshaler& m) {
    auto hdlg = static_cast<windowing::HWND>(m.arg(0));
    auto control_id = static_cast<u32>(m.arg(1));
    auto raw_value = static_cast<u32>(m.arg(2));
    bool is_signed = m.arg(3) != 0;

    std::wstring text;
    if (is_signed) {
        i32 signed_value = static_cast<i32>(raw_value);
        text = std::to_wstring(signed_value);
    } else {
        text = std::to_wstring(raw_value);
    }

    bool ok = false;
    if (m_dialog_mgr) {
        if (m_dialog_mgr->get_dlg_item(hdlg, control_id) == windowing::NULL_HWND) {
            // Measured: ERROR_CONTROL_ID_NOT_FOUND; success clears the error.
            set_last_error(1421);
            m.set_return(0u);
            return;
        }
        ok = m_dialog_mgr->set_dlg_item_text(hdlg, control_id, text);
        if (ok) set_last_error(0);
    }

    std::string utf8;
    utf8.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch >= 0 && ch <= 0x7F) {
            utf8.push_back(static_cast<char>(ch));
        } else {
            utf8.push_back('?');
        }
    }

    VITREO_TRACE("[user32_ext] SetDlgItemInt: dlg=0x%x id=%u value=%s signed=%d -> %d\n",
                 hdlg,
                 control_id,
                 utf8.c_str(),
                 is_signed ? 1 : 0,
                 ok ? 1 : 0);
    m.set_return(ok ? 1 : 0);
}

} // namespace vitreo::winapi
