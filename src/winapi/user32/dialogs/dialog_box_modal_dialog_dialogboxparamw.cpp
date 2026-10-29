// SPDX-License-Identifier: LGPL-2.1-only

/// @file dialog.cpp
/// @brief Dialog box management implementation — template-driven

#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/dialogs/dialog_internal.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/user32/messages/message_time.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/parser/pe_structures.hpp"


#include <cstdio>
#include <cstdlib>
#include "windowing/model/window_types.hpp"

#include <cmath>
#include <cstdlib>
#include <unistd.h>  // usleep
#include <optional>
#include <array>

/// @file dialog_box_modal_dialog_dialogboxparamw.cpp
/// @brief dialog_box  — modal dialog (DialogBoxParamW)
///
/// Closed cut from dialog.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// dialog_box  — modal dialog (DialogBoxParamW)
// =============================================================================

i32 DialogManager::dialog_box(u64 hinstance, u64 template_id,
                              windowing::HWND parent,
                              u64 dlgproc, u64 init_param,
                              bool require_template) {

    VITREO_TRACE( "[dialog] DialogBoxParamW: template=%llu parent=0x%x "
                 "dlgproc=0x%llx initParam=0x%llx\n",
                 static_cast<unsigned long long>(template_id),
                 parent,
                 static_cast<unsigned long long>(dlgproc),
                 static_cast<unsigned long long>(init_param));

    if (!m_dlgproc_callback) {
        VITREO_TRACE( "[dialog] DialogBoxParamW: no dlgproc callback set!\n");
        return -1;
    }

    // Reset modal state
    m_modal_ended  = false;
    m_modal_result = 0;

    // Create the dialog (reuse CreateDialogParamW logic).
    // WM_INITDIALOG is sent synchronously by create_dialog (default),
    // matching the real Win32 behavior where DialogBoxParam sends
    // WM_INITDIALOG before the modal loop starts.  This ensures all
    // page setup (text, subclassing) completes before any messages
    // arrive from the queue.
    windowing::HWND dlg_hwnd = create_dialog(hinstance, template_id,
                                              parent, dlgproc, init_param,
                                              false, require_template,
                                              true /* modal */);
    if (dlg_hwnd == windowing::NULL_HWND) {
        VITREO_TRACE( "[dialog] DialogBoxParamW: create_dialog failed\n");
        return -1;
    }

    if (diag_logs_enabled()) {
        const auto* win = m_hwnd_mgr.find(dlg_hwnd);
        std::fprintf(stderr, "[dlg-diag] modal hwnd=0x%x template=0x%llx parent=0x%x valid=%d ended=%d visible=%d style=0x%x rect=(%d,%d,%d,%d)\n",
            dlg_hwnd, static_cast<unsigned long long>(template_id), parent,
            win != nullptr, m_modal_ended, win ? win->visible : false,
            win ? win->style : 0, win ? win->rect.left : 0, win ? win->rect.top : 0,
            win ? win->rect.right : 0, win ? win->rect.bottom : 0);
    }

    // DialogBox disables an enabled owner for the lifetime of the modal
    // dialog, then restores it before returning. Preserve a pre-disabled
    // owner instead of enabling it unconditionally.
    bool restore_owner = false;
    u64 owner_wndproc = 0;
    if (parent != windowing::NULL_HWND && parent != windowing::HWND_DESKTOP) {
        if (windowing::WindowData* owner = m_hwnd_mgr.find(parent);
            owner != nullptr && owner->enabled) {
            restore_owner = true;
            owner_wndproc = owner->wndproc;
            owner->enabled = false;
            (void)m_control_host.set_control_enabled(parent, false);
            if (owner_wndproc != 0) {
                (void)m_dlgproc_callback(parent, windowing::WM::ENABLE, 0, 0,
                                         owner_wndproc);
            }
        }
    }

    // Store the modal dialog HWND for alias fallback
    m_current_modal_hwnd = dlg_hwnd;

    VITREO_TRACE( "[dialog] DialogBoxParamW: dialog created hwnd=0x%x, "
                 "entering modal loop\n", dlg_hwnd);

    // ---- Modal message loop ----
    // Process messages and dispatch them to the DLGPROC until EndDialog is called.
    // WM_INITDIALOG was already sent synchronously by create_dialog().
    const u32 modal_thread_id = ThreadManager::get_current_thread_id_static();
    for (;;) {
        // Pump Cocoa events
        m_control_host.pump_events(m_queue);

        // External exit signal: e.g. the guest queued a chain-load via
        // ShellExecute/CreateProcess. In that model the current guest must
        // exit so the next executable can run, so we release the modal pump.
        if (!m_modal_ended && m_modal_exit_predicate && m_modal_exit_predicate()) {
            VITREO_TRACE("[dialog] modal pump: external exit signal, ending dialog\n");
            end_dialog(dlg_hwnd, 1 /*IDOK*/);
            break;
        }

        windowing::MSG msg{};
        bool got = m_queue.get_for_thread(
            msg, modal_thread_id, m_hwnd_mgr, 0, 0, 0);

        if (!got) {
            windowing::TreeNotify notify;
            if (m_control_host.pop_tree_notify(notify)) {
                GuestAddr payload = marshal_modal_tree_notify(m_memory, notify);
                if (payload != 0) {
                    msg.hwnd = notify.parent;
                    msg.message = 0x004E; // WM_NOTIFY
                    msg.wParam = notify.ctrl_id;
                    msg.lParam = payload;
                    got = true;
                }
            }
        }

        if (got) {
            if (msg.message == windowing::WM::NOTIFY) {
                windowing::HWND tab_control = windowing::NULL_HWND;
                bool selection_changing = false;
                if (windowing::decode_tab_notify_lparam(
                        msg.lParam, &tab_control, &selection_changing)) {
                    msg.lParam = marshal_modal_tab_notify(
                        m_memory, msg.lParam,
                        static_cast<u32>(msg.wParam));
                }
            }
            VITREO_TRACE( "[dialog] modal dequeue: hwnd=0x%x msg=0x%x wParam=0x%llx lParam=0x%llx\n",
                         msg.hwnd, msg.message,
                         static_cast<unsigned long long>(msg.wParam),
                         static_cast<unsigned long long>(msg.lParam));

            // Detect WM_SIZE dispatched through modal loop
            if (msg.message == windowing::WM::SIZE && diag_logs_enabled()) {
                std::fprintf(stderr,
                    "[msg-diag] modal dequeue WM_SIZE: hwnd=0x%x lParam=0x%llx\n",
                    msg.hwnd, static_cast<unsigned long long>(msg.lParam));
            }
        }

        if (!got) {
            // Queue empty and no quit — check if EndDialog was called
            if (m_modal_ended) {
                break;
            }
            if (m_queue.quit_posted(modal_thread_id)) {
                // Consume the quit flag via get() so a nested message loop
                // does not see a stale WM_QUIT.
                windowing::MSG quit_msg{};
                (void)m_queue.get_for_thread(
                    quit_msg, modal_thread_id, m_hwnd_mgr, 0, 0, 0);
                VITREO_TRACE( "[dialog] DialogBoxParamW: quit posted (consumed)\n");
                // Re-post WM_QUIT so the application's outer loop sees it
                // after DialogBoxParamW returns (Win32 contract).
                m_queue.post_quit(
                    static_cast<int>(quit_msg.wParam), modal_thread_id);
                break;
            }
            // Synthesize WM_TIMER when the queue is empty — same contract as
            // GetMessage. A SetTimer armed from the dialog (for example
            // SetTimer(1, 1000) in WM_INITDIALOG) must fire here or the
            // dialog proc never sees it.
            if (m_user32_ext_mgr) {
                windowing::HWND timer_hwnd = 0;
                u64 timer_id = 0;
                GuestAddr timer_callback = 0;
                if (m_user32_ext_mgr->find_expired_timer(
                        timer_hwnd, timer_id, timer_callback, modal_thread_id)) {
                    msg = make_timer_message(timer_hwnd, timer_id,
                                             timer_callback);
                    got = true;
                    VITREO_TRACE(
                        "[dialog] modal synthesized WM_TIMER hwnd=0x%x id=%llu\n",
                        timer_hwnd, static_cast<unsigned long long>(timer_id));
                }
            }
            if (!got) {
                // Block briefly while allowing AppKit to run; this helps prevent
                // stalls when Cocoa needs a runloop tick while we're waiting.
                m_control_host.wait_events(m_queue, 10);
                continue;
            }
        }

        stamp_message_time(msg);
        if (m_user32_ext_mgr) {
            m_user32_ext_mgr->note_retrieved_queue_message(msg);
        }

        // WM_QUIT: exit loop.
        // Re-post so the application's outer message loop also sees it
        // (Win32 contract: DialogBox re-posts WM_QUIT).
        if (msg.message == windowing::WM::QUIT) {
            VITREO_TRACE( "[dialog] DialogBoxParamW: WM_QUIT received, re-posting\n");
            m_queue.post_quit(static_cast<int>(msg.wParam), modal_thread_id);
            break;
        }

        // Learn MessageProc: a dialog box's modal loop hands each message to
        // WH_MSGFILTER with MSGF_DIALOGBOX; a nonzero return consumes it.
        constexpr i32 MSGF_DIALOGBOX = 0;
        if (m_user32_ext_mgr &&
            m_user32_ext_mgr->call_msg_filter(msg, MSGF_DIALOGBOX)) {
            continue;
        }

        // Cocoa-generated messages may arrive with hwnd=0.
        // Normalize NULL HWND to the modal dialog handle, but NEVER for
        // destructive messages — a stray WM_DESTROY with hwnd=0 must not
        // be dispatched to the main dialog.
        if (msg.hwnd == windowing::NULL_HWND) {
            if (msg.message == windowing::WM::DESTROY
             || msg.message == windowing::WM::NCDESTROY
             || msg.message == windowing::WM::CLOSE) {
                continue; // silently drop
            }
            msg.hwnd = dlg_hwnd;
        } else if (!m_hwnd_mgr.is_valid(msg.hwnd)) {
            // Message targets a destroyed/invalid window — discard it.
            // This prevents WM_DESTROY for destroyed child windows from
            // being dispatched to the main dialog's DlgProc.
            continue;
        }

        // Determine which dialog proc to dispatch to.
        // Only walk up the parent chain for notification messages (WM_COMMAND, WM_NOTIFY).
        // For other messages targeting child controls, skip dispatch — the modal
        // dialog proc should not receive WM_SHOWWINDOW/WM_SIZE/WM_MOVE for children.
        windowing::HWND dispatch_hwnd = dlg_hwnd;
        u64 proc = dlgproc;

        const windowing::WindowData* win = m_hwnd_mgr.find(msg.hwnd);
        if (win) {
            if (win->wndproc != 0) {
                // Target has its own wndproc (it's a dialog)
                proc = win->wndproc;
                dispatch_hwnd = msg.hwnd;
            } else {
                // Target is a child control (no wndproc).
                // Only propagate WM_COMMAND and WM_NOTIFY to parent dialog.
                bool is_notification = (msg.message == windowing::WM::COMMAND)
                                    || (msg.message == 0x004E); // WM_NOTIFY = 0x4E
                if (!is_notification) {
                    // Skip non-notification messages for child controls
                    continue;
                }
                // Walk up parent chain to find dialog with a wndproc
                windowing::HWND cur = win->parent;
                while (cur != windowing::HWND_DESKTOP && cur != windowing::NULL_HWND) {
                    const windowing::WindowData* parent_win = m_hwnd_mgr.find(cur);
                    if (!parent_win) break;
                    if (parent_win->wndproc != 0) {
                        proc = parent_win->wndproc;
                        dispatch_hwnd = cur;
                        break;
                    }
                    cur = parent_win->parent;
                }
            }
        }

        if (proc != 0) {
            // Trace WM_COMMAND dispatched through modal loop
            if (msg.message == windowing::WM::COMMAND && diag_logs_enabled()) {
                std::fprintf(stderr,
                    "[btn-diag] MODAL dispatch WM_COMMAND: dlg=0x%x msg_hwnd=0x%x dispatch=0x%x "
                    "id=%u notify=%u proc=0x%llx\n",
                    dlg_hwnd, msg.hwnd, dispatch_hwnd,
                    static_cast<unsigned>(msg.wParam & 0xFFFF),
                    static_cast<unsigned>((msg.wParam >> 16) & 0xFFFF),
                    static_cast<unsigned long long>(proc));
            }

            VITREO_TRACE( "[dialog] modal dispatch: msg_hwnd=0x%x dispatch=0x%x msg=0x%x "
                         "wParam=0x%llx proc=0x%llx\n",
                         msg.hwnd, dispatch_hwnd, msg.message,
                         static_cast<unsigned long long>(msg.wParam),
                         static_cast<unsigned long long>(proc));

            u64 result = m_dlgproc_callback(
                dispatch_hwnd, msg.message, msg.wParam, msg.lParam, proc
            );

            // Trace modal dispatch result for WM_COMMAND
            if (msg.message == windowing::WM::COMMAND && diag_logs_enabled()) {
                std::fprintf(stderr,
                    "[btn-diag] MODAL dlgproc result: hwnd=0x%x id=%u -> 0x%llx modal_ended=%d\n",
                    dispatch_hwnd,
                    static_cast<unsigned>(msg.wParam & 0xFFFF),
                    static_cast<unsigned long long>(result),
                    m_modal_ended ? 1 : 0);
            }

            // Default dialog behavior: if the window is asked to close and guest
            // doesn't end the modal loop itself, end with IDCANCEL.
            if (msg.message == windowing::WM::CLOSE && !m_modal_ended) {
                end_dialog(dlg_hwnd, 2 /*IDCANCEL*/);
            }
        }

        // Check if EndDialog was called during dispatch
        if (m_modal_ended) {
            break;
        }
    }

    // EndDialog defers destruction until the dialog procedure has returned.
    // Closing the native view alone leaves the HWND (and its timers/userdata)
    // alive after the caller has released the dialog's state.
    auto destroy_dialog_tree = [&](auto&& self, windowing::HWND hwnd) -> void {
        const auto* window = m_hwnd_mgr.find(hwnd);
        if (!window) return;
        const auto proc = window->wndproc;
        if (proc) m_dlgproc_callback(hwnd, windowing::WM::DESTROY, 0, 0, proc);
        for (auto child : m_hwnd_mgr.enum_children(hwnd)) self(self, child);
        for (auto candidate : m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP)) {
            const auto* owned = m_hwnd_mgr.find(candidate);
            if (candidate != hwnd && owned && owned->owner == hwnd)
                self(self, candidate);
        }
        if (proc) m_dlgproc_callback(hwnd, windowing::WM::NCDESTROY, 0, 0, proc);
        m_control_host.destroy_window(hwnd);
        m_hwnd_mgr.destroy(hwnd);
        ipc::hwnd_registry_unpublish(static_cast<u32>(hwnd));
    };
    destroy_dialog_tree(destroy_dialog_tree, dlg_hwnd);
    m_current_modal_hwnd = windowing::NULL_HWND;

    if (restore_owner) {
        if (windowing::WindowData* owner = m_hwnd_mgr.find(parent)) {
            owner->enabled = true;
            (void)m_control_host.set_control_enabled(parent, true);
            if (owner_wndproc != 0) {
                (void)m_dlgproc_callback(parent, windowing::WM::ENABLE, 1, 0,
                                         owner_wndproc);
            }
            (void)m_control_host.activate_window(parent);
        }
    }

    VITREO_TRACE( "[dialog] DialogBoxParamW: modal loop ended, result=%d\n",
                 m_modal_result);

    return m_modal_result;
}

} // namespace vitreo::winapi
