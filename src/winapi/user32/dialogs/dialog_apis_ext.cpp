// SPDX-License-Identifier: LGPL-2.1-only

/// @file dialog_apis_ext.cpp
/// @brief Additional Dialog APIs banner.
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

// ----- Dialog APIs -----

void User32ExtManager::impl_CreateDialogIndirectParamA(Marshaler& m) {
    // MS Learn (CreateDialogIndirectParamA): dialog templates store title /
    // control strings as Unicode for both A and W entry points. Same parser
    // as DialogBoxIndirectParamA → W.
    impl_CreateDialogIndirectParamW(m);
}

void User32ExtManager::impl_CreateDialogIndirectParamW(Marshaler& m) {
    u32 hinstance = static_cast<u32>(m.arg(0));
    u64 template_raw = m.arg(1);
    GuestAddr lpTemplate = m.arg_ptr(1);
    u64 template_id = (lpTemplate != 0)
        ? static_cast<u64>(lpTemplate)
        : template_raw;
    u32 parent = static_cast<u32>(m.arg(2));
    u64 dlgproc = m.arg(3);
    u64 init_param = m.arg(4);

    if (m_dialog_mgr) {
        // Keep the hook bridge local to the USER32 entry point as well as the
        // manager wiring path. This guarantees that dialogs created after a
        // late manager hookup still receive HCBT_CREATEWND.
        m_dialog_mgr->set_creation_hook(
            [this](windowing::HWND hwnd, u32 style) {
                return dispatch_cbt_create_hook(hwnd, style);
            });
        const auto hwnd = m_dialog_mgr->create_dialog(
            hinstance,
            template_id,
            static_cast<windowing::HWND>(parent),
            dlgproc,
            init_param,
            false,
            /*require_template=*/true
        );
        if (hwnd != windowing::NULL_HWND) {
            m_active_window = static_cast<u32>(hwnd);
            VITREO_TRACE("[u32ext] CreateDialogIndirectParamW hinst=0x%x tmpl_raw=0x%llx tmpl_ptr=0x%llx parent=0x%x dlgproc=0x%llx -> hwnd=0x%x (dialog_mgr)\n",
                         hinstance,
                         static_cast<unsigned long long>(template_raw),
                         static_cast<unsigned long long>(lpTemplate),
                         parent,
                         static_cast<unsigned long long>(dlgproc),
                         static_cast<u32>(hwnd));
            m.set_return(static_cast<u32>(hwnd));
            return;
        }
    }

    // No dialog was created (create_dialog set the last error): NULL, never
    // a handle that names no window.
    m.set_return(0u);
}

void User32ExtManager::impl_GetNextDlgTabItem(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
    VITREO_TRACE("[u32ext] GetNextDlgTabItem -> 0\n");
    m.set_return(0u);
}

void User32ExtManager::impl_GetNextDlgGroupItem(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
    VITREO_TRACE("[u32ext] GetNextDlgGroupItem -> 0\n");
    m.set_return(0u);
}

} // namespace vitreo::winapi
