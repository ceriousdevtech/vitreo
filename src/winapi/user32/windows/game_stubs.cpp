// SPDX-License-Identifier: LGPL-2.1-only

/// @file game_stubs.cpp
/// @brief Minesweeper / classic-game stubs.
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

// =============================================================================
// Minesweeper / Classic Game Stubs
// =============================================================================




void User32ExtManager::impl_ReleaseCapture(Marshaler& m) {
    // BOOL ReleaseCapture(void). s2msg_input_probe: releasing sends the old
    // capture window WM_CAPTURECHANGED (lParam NULL) and sets the last error
    // to 0; with no capture it is TRUE with the last error untouched.
    const windowing::HWND old = m_hwnd_mgr.get_capture();
    if (old != windowing::NULL_HWND && window_in_caller_queue(old)) {
        m_hwnd_mgr.release_capture();
        m_host.set_mouse_capture(windowing::NULL_HWND);
        constexpr u32 WM_CAPTURECHANGED = 0x0215;
        if (m_msgsend_mgr != nullptr && m_hwnd_mgr.is_valid(old)) {
            (void)m_msgsend_mgr->send_message(old, WM_CAPTURECHANGED, 0, 0);
        }
        set_last_error(0);
    }
    VITREO_TRACE("[u32ext] ReleaseCapture() old=0x%x\n", old);
    m.set_return(1); // TRUE
}

} // namespace vitreo::winapi
