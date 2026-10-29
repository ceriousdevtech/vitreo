// SPDX-License-Identifier: LGPL-2.1-only

/// @file class_info.cpp
/// @brief Get/SetClassLong*, GetClassName, IsChild, and GetDlgCtrlID.
///
/// Class atom metadata and the parent/child identity queries that dialog
/// and subclassing code reach through GetClassLongPtr / GetClassName.
///
/// Closed cut from user32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/user32_errors.hpp"
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

// ----- Class APIs -----

void User32ExtManager::impl_GetClassLongW(Marshaler& m) { class_long_get(m, 4); }

void User32ExtManager::impl_GetClassLongPtrW(Marshaler& m) {
    class_long_get(m, m_is_64bit ? 8u : 4u);
}

void User32ExtManager::impl_SetClassLongPtrW(Marshaler& m) {
    class_long_set(m, m_is_64bit ? 8u : 4u);
}

std::wstring User32ExtManager::window_class_name(windowing::HWND hwnd,
                                                 const windowing::WindowData& win) {
    // The desktop window's class is the system class "#32769" (Learn: About
    // Window Classes; measured getclassnamew_desktop).
    if (hwnd == windowing::HWND_DESKTOP) return L"#32769";
    if (const WindowClassData* cls = window_class(win)) return cls->class_name;
    if (!win.class_name.empty()) return win.class_name;
    if (win.is_dialog) return L"#32770";
    return {};
}

void User32ExtManager::get_class_name(Marshaler& m, bool ansi) {
    // int GetClassName(HWND, LPTSTR lpClassName, int nMaxCount): the number
    // of characters copied, excluding the NUL (Learn). Measured
    // (s1win_api_probe getclassname*): the name is truncated to
    // nMaxCount - 1 characters plus NUL; nMaxCount <= 1 fails with
    // ERROR_INSUFFICIENT_BUFFER and leaves the buffer untouched.
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const i32 max_count = static_cast<i32>(m.arg(2));
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    const std::wstring name = window_class_name(hwnd, *win);
    if (max_count <= 1 || buffer == 0) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0u);
        return;
    }
    const i32 copy = std::min(static_cast<i32>(name.size()), max_count - 1);
    for (i32 i = 0; i < copy; ++i) {
        const wchar_t ch = name[static_cast<usize>(i)];
        if (ansi) {
            (void)m.write_u8(buffer + static_cast<u32>(i),
                             static_cast<u8>(ch < 0x80 ? ch : L'?'));
        } else {
            (void)m.write_u16(buffer + static_cast<u32>(i * 2), static_cast<u16>(ch));
        }
    }
    if (ansi) (void)m.write_u8(buffer + static_cast<u32>(copy), 0);
    else (void)m.write_u16(buffer + static_cast<u32>(copy * 2), 0);
    VITREO_TRACE("[u32ext] GetClassName%c hwnd=0x%x -> %d '%ls'\n",
                 ansi ? 'A' : 'W', hwnd, copy, name.c_str());
    m.set_return(static_cast<u32>(copy));
}

void User32ExtManager::impl_GetClassNameW(Marshaler& m) { get_class_name(m, false); }

void User32ExtManager::impl_IsChild(Marshaler& m) {
    // MS Learn: TRUE if hWnd is a child/descendant of hWndParent.
    const u32 hWndParent = static_cast<u32>(m.arg(0));
    const u32 hWnd = static_cast<u32>(m.arg(1));
    if (hWndParent == 0 || hWnd == 0 || hWndParent == hWnd) {
        m.set_return(0u);
        return;
    }
    const windowing::WindowData* child = m_hwnd_mgr.find(
        static_cast<windowing::HWND>(hWnd));
    bool is_child = false;
    for (u32 guard = 0; child && guard < 64; ++guard) {
        if (child->parent == 0 ||
            child->parent == windowing::HWND_DESKTOP) {
            break;
        }
        if (child->parent == static_cast<windowing::HWND>(hWndParent)) {
            is_child = true;
            break;
        }
        child = m_hwnd_mgr.find(child->parent);
    }
    VITREO_TRACE("[u32ext] IsChild parent=0x%x child=0x%x -> %u\n",
                 hWndParent, hWnd, is_child ? 1u : 0u);
    m.set_return(is_child ? 1u : 0u);
}

void User32ExtManager::impl_GetDlgCtrlID(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const windowing::WindowData* window = m_hwnd_mgr.find(hwnd);
    if (!window) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        VITREO_TRACE(
            "[u32ext] GetDlgCtrlID hwnd=0x%x -> 0 "
            "(ERROR_INVALID_WINDOW_HANDLE)\n",
            hwnd);
        m.set_return(0u);
        return;
    }

    VITREO_TRACE(
        "[u32ext] GetDlgCtrlID hwnd=0x%x -> %u\n",
        hwnd, window->ctrl_id);
    m.set_return(window->ctrl_id);
}

// UINT RealGetWindowClassW(HWND hwnd, PWSTR pszType, UINT cchType)
//
// Learn: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-realgetwindowclassw
// Retrieves the *base* class of a window — the real class behind any
// superclassing an application layered on top. Vitreo's class manager records
// the registered class directly and never superclasses, so the base class and
// the class name are the same string.
void User32ExtManager::impl_RealGetWindowClassW(Marshaler& m) {
    impl_GetClassNameW(m);
}

void User32ExtManager::impl_RealGetWindowClassA(Marshaler& m) {
    impl_GetClassNameA(m);
}

// BOOL IsHungAppWindow(HWND hwnd)
//
// Learn: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-ishungappwindow
// TRUE when the window's thread has stopped answering messages. Vitreo
// dispatches guest messages on the owning guest thread, and a guest that has
// stopped pumping stops the whole process, so a live window is never hung.
void User32ExtManager::impl_IsHungAppWindow(Marshaler& m) {
    const auto hwnd = static_cast<u32>(m.arg(0));
    const windowing::WindowData* win =
        m_hwnd_mgr.find(static_cast<windowing::HWND>(hwnd));
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    m.set_return(0u);
}

// ---- Touch input -------------------------------------------------------
//
// Learn: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registertouchwindow
// Registration is a per-window flag and succeeds on machines with no
// digitizer; IsTouchWindow reports that flag. Because no digitizer exists,
// no WM_TOUCH is ever posted, so GetTouchInputInfo can only be reached with a
// handle that was never issued — ERROR_INVALID_HANDLE, as on Windows.
bool User32ExtManager::touch_window_access(u32 hwnd, bool require_owner,
                                          const char* api_name) {
    const auto owner = m_hwnd_mgr.owner_thread_id(static_cast<windowing::HWND>(hwnd));
    if (!m_hwnd_mgr.is_valid(static_cast<windowing::HWND>(hwnd))) {
        if (ipc::hwnd_registry_lookup(hwnd)) {
            if (require_owner) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
            } else {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true))
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!IsTouchWindow: peer registration state\n");
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            }
        } else {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        }
        return false;
    }
    if (require_owner &&
        (hwnd == windowing::HWND_DESKTOP || !owner ||
         *owner != ThreadManager::get_current_thread_id_static())) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }
    {
        std::lock_guard lock(m_user_object_mutex);
        // The current desktop model does not associate HWNDs with nondefault
        // desktops. Refuse that case rather than fabricate desktop permission.
        if (m_thread_desktop != kDefaultDesktopHandle) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true))
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!%s: nondefault desktop association\n", api_name);
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            return false;
        }
    }
    return true;
}

void User32ExtManager::impl_RegisterTouchWindow(Marshaler& m) {
    const auto hwnd = static_cast<u32>(m.arg(0));
    const u32 flags = static_cast<u32>(m.arg(1));
    if (!touch_window_access(hwnd, true, "RegisterTouchWindow")) {
        m.set_return(0u);
        return;
    }
    // TWF_FINETOUCH | TWF_WANTPALM only (s2msg_input_probe
    // registertouchwindow_bad_flags: FALSE / ERROR_INVALID_FLAGS).
    if ((flags & ~0x3u) != 0) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        m.set_return(0u);
        return;
    }
    {
        std::lock_guard lock(m_window_state_mutex);
        m_touch_windows[hwnd] = flags;
    }
    m.set_return(1u);
}

void User32ExtManager::impl_UnregisterTouchWindow(Marshaler& m) {
    const auto hwnd = static_cast<u32>(m.arg(0));
    if (!touch_window_access(hwnd, true, "UnregisterTouchWindow")) {
        m.set_return(0u);
        return;
    }
    {
        std::lock_guard lock(m_window_state_mutex);
        m_touch_windows.erase(hwnd);
    }
    m.set_return(1u);
}

void User32ExtManager::impl_IsTouchWindow(Marshaler& m) {
    const auto hwnd = static_cast<u32>(m.arg(0));
    const GuestAddr flags_out = m.arg_ptr(1);
    if (!touch_window_access(hwnd, false, "IsTouchWindow")) {
        m.set_return(0u);
        return;
    }
    std::optional<u32> flags;
    {
        std::lock_guard lock(m_window_state_mutex);
        if (const auto it = m_touch_windows.find(hwnd); it != m_touch_windows.end())
            flags = it->second;
    }
    if (flags && flags_out != 0 && m.write_u32(flags_out, *flags).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    m.set_return(flags ? 1u : 0u);
}

void User32ExtManager::impl_GetTouchInputInfo(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    m.set_return(0u);
}

} // namespace vitreo::winapi
