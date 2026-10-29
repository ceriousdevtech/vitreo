// SPDX-License-Identifier: LGPL-2.1-only

/// @file class_charset.cpp
/// @brief The ANSI/Unicode window-class charset entry points: IsWindowUnicode,
///        GetClassLongA and GetClassNameA. Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/hwnd_registry.hpp"

namespace vitreo::winapi {

void User32ExtManager::impl_IsWindowUnicode(Marshaler& m) {
    // Learn IsWindowUnicode: TRUE when the window is a native Unicode window.
    // The charset comes from the class (RegisterClass*A/W), the creating
    // CreateWindowEx for a system class, and the last SetWindowLongPtrA/W
    // GWLP_WNDPROC (measured, s1win_class_probe / s1win_api_probe).
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (const windowing::WindowData* win = m_hwnd_mgr.find(hwnd)) {
        m.set_return(win->unicode ? 1u : 0u);
        return;
    }
    if (hwnd != 0 && ipc::hwnd_registry_is_window(static_cast<u32>(hwnd))) {
        // Another capsule process's window: its charset lives in that
        // process. Cross-process HWNDs are reported as Unicode.
        m.set_return(1u);
        return;
    }
    set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
    m.set_return(0u);
}

void User32ExtManager::impl_GetClassLongA(Marshaler& m) { class_long_get(m, 4); }

void User32ExtManager::impl_GetClassNameA(Marshaler& m) { get_class_name(m, true); }

} // namespace vitreo::winapi
