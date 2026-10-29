// SPDX-License-Identifier: LGPL-2.1-only

/// @file message_send.cpp
/// @brief PostQuitMessage/PostMessageW/SendMessageW implementation (WIN32-055)

#include "message_send.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/model/diag_flags.hpp"
#include <cstdlib>
#include <string>
#include <cstdio>
#include <vector>
#include <pthread.h>

/// @file send_message.cpp
/// @brief send_message
///
/// Closed cut from message_send.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// send_message
// -----------------------------------------------------------------------------

u64 MessageSendManager::send_message(
    windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam
) {
    if (windowing::diag::input_diag() &&
        ((msg >= 0x00A0 && msg <= 0x00AF) || msg == 0x0112 ||
         msg == 0x0231 || msg == 0x0232)) {
        std::fprintf(stderr,
                     "[input] SendMessage hwnd=0x%x msg=0x%04x wParam=0x%llx\n",
                     hwnd, msg, static_cast<unsigned long long>(wParam));
    }

    if (hwnd == windowing::NULL_HWND) {
        // Learn: hWnd identifies the window that receives the message. NULL is
        // not a window (SendMessageTimeoutW probe smto_null_hwnd → 0 /
        // ERROR_INVALID_WINDOW_HANDLE). Never redirect to an arbitrary top-level.
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    VITREO_TRACE(
                 "[send] SendMessage hwnd=0x%x msg=0x%x wParam=0x%llx lParam=0x%llx\n",
                 hwnd, msg,
                 static_cast<unsigned long long>(wParam),
                 static_cast<unsigned long long>(lParam));

    const auto* win = m_hwnd_mgr.find(hwnd);

    // Set by the cross-thread delivery below when this call runs on the
    // receiving thread for another thread's SendMessage.
    static thread_local bool t_cross_thread_delivery = false;
    const bool cross_thread = t_cross_thread_delivery;
    t_cross_thread_delivery = false;

    const u32 sender = ThreadManager::get_current_thread_id_static();
    if (win && win->owner_thread_id != 0 && win->owner_thread_id != sender) {
        std::function<void(std::function<bool()>)> host_wait;
        // nextEvent before finishLaunching hangs; without a launched app the
        // MessageQueue CV is the only safe wait (SendMessage may block forever).
        if (pthread_main_np() != 0 && m_bridge.is_app_launched()) {
            host_wait = [this](std::function<bool()> ready) {
                m_bridge.wait_events(m_queue, 16, std::move(ready));
            };
        }
        return m_queue.send_to_thread(sender, win->owner_thread_id,
            [this, hwnd, msg, wParam, lParam] {
                // Re-resolve the HWND on the receiver; destruction may have
                // happened while the sending thread was waiting.
                t_cross_thread_delivery = true;
                return send_message(hwnd, msg, wParam, lParam);
            }, std::move(host_wait));
    }

    // WH_CALLWNDPROC / WH_CALLWNDPROCRET run on the receiving thread around
    // every message SendMessage delivers (never for DispatchMessage:
    // s2msg_queue_probe callwndproc_dispatch n=0).
    if (win != nullptr && m_call_wndproc_hook) {
        m_call_wndproc_hook(hwnd, msg, wParam, lParam, cross_thread, false, 0);
        const u64 result = deliver_sent_message(hwnd, msg, wParam, lParam);
        m_call_wndproc_hook(hwnd, msg, wParam, lParam, cross_thread, true, result);
        return result;
    }
    return deliver_sent_message(hwnd, msg, wParam, lParam);
}

u64 MessageSendManager::deliver_sent_message(
    windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam
) {
    const auto* win = m_hwnd_mgr.find(hwnd);

    // Only intercept PBM_* for windows without a wndproc (controls such as
    // a progress bar). A window with a wndproc uses WM_USER+n for its own
    // messages. Routing PBM_* from those windows to children swallows the
    // WM_USER+n messages the dialog proc must receive.
    if (!win || win->wndproc == 0) {
        u64 result = 0;
        if (handle_control_message(hwnd, msg, wParam, lParam, result)) {
            return result;
        }
    }

    if (!win) {
        // s2msg_queue_probe send_invalid: 0 / ERROR_INVALID_WINDOW_HANDLE.
        VITREO_TRACE( "[send] hwnd not found -> return 0\n");
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    u64 wndproc = win->wndproc;
    if (wndproc == 0) {
        VITREO_TRACE( "[send] no wndproc -> return 0\n");
        return 0;
    }
    if (!m_wndproc_callback) {
        VITREO_TRACE( "[send] no callback -> return 0\n");
        return 0;
    }
    return m_wndproc_callback(hwnd, msg, wParam, lParam, wndproc);
}

} // namespace vitreo::winapi
