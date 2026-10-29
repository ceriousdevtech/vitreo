// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_activation.mm
/// @brief Foreground rights, key-window activation, and occlusion.
///
/// Win32 decides which process may take the foreground and which window is
/// active; AppKit reports key-window and application-active changes. These
/// functions grant foreground rights to a launched process tree, turn
/// AppKit's key and active notifications into the guest's activation
/// state, and record which windows are occluded.
///
/// Split out of cocoa_bridge.mm, which was 7,624 lines. These are CocoaBridge
/// methods defined in a separate translation unit; the class is unchanged.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/model/message_box_layout.hpp"
#include "windowing/model/diag_flags.hpp"
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_control_delegates.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/messages/input_latency.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/cocoa/window_presentation.hpp"
#include "windowing/cocoa/app_icon_presentation.hpp"
#include "graphics/common/perf_diag.hpp"
#include "graphics/common/surface_presenter.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cmath>
#include <limits>
#include <algorithm>
#include <CoreFoundation/CFRunLoop.h>
#include <Block.h>
#include <deque>
#include <mutex>
#include <atomic>
#include <CoreGraphics/CGEventSource.h>
#include <libproc.h>
#include <cerrno>
#include <signal.h>
#include <mach/mach_time.h>
#include "windowing/cocoa/cocoa_bridge_internal.h"

namespace vitreo::windowing {

bool CocoaBridge::pid_descends_from(i32 pid, i32 ancestor,
                                    const std::function<i32(i32)>& parent_of) {
    if (pid <= 0 || ancestor <= 0) return false;
    i32 current = pid;
    // Process trees are shallow; the bound also stops a reused-pid cycle.
    for (int depth = 0; depth < 64 && current > 1; ++depth) {
        if (current == ancestor) return true;
        const i32 parent = parent_of ? parent_of(current) : 0;
        if (parent == current) return false;
        current = parent;
    }
    return current == ancestor;
}

bool CocoaBridge::process_may_set_foreground() const {
    // The host's answer: the guest's own activation bookkeeping can claim
    // activation (SetActiveWindow) while the app is in the background.
    if (m_holds_start_grant.load(std::memory_order_acquire)) return true;
    __block bool active = false;
    vtr_run_on_main_sync(^{
        active = [NSApp isActive] == YES;
    });
    return foreground_request_allowed(active, false);
}

namespace {
vitreo::i32 host_parent_pid(vitreo::i32 pid) {
    struct proc_bsdshortinfo info{};
    const int bytes = proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &info,
                                   static_cast<int>(sizeof(info)));
    if (bytes != static_cast<int>(sizeof(info))) return 0;
    return static_cast<vitreo::i32>(info.pbsi_ppid);
}
} // namespace

void CocoaBridge::grant_foreground_to_process_tree(i32 child_pid) {
    if (child_pid <= 0) return;
    // Before macOS 14 an activation request needs no yield.
    if (@available(macOS 14.0, *)) {
    } else {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        // Only the active application's yield is honoured.
        if (![NSApp isActive]) return;
        if (std::find(m_foreground_grant_roots.begin(),
                      m_foreground_grant_roots.end(),
                      child_pid) == m_foreground_grant_roots.end()) {
            m_foreground_grant_roots.push_back(child_pid);
        }
        VITREO_TRACE("[cocoa] foreground grant -> process tree %d\n",
                     static_cast<int>(child_pid));
        tick_foreground_grants();
        if (m_foreground_grant_roots.empty() || m_foreground_grant_timer) {
            return;
        }
        // A descendant becomes an NSRunningApplication when its
        // NSApplication launches, after this process spawned it; look for it
        // until it does (cheap: one runningApplications walk per tick).
        NSTimer* timer = [NSTimer
            scheduledTimerWithTimeInterval:0.1
                                   repeats:YES
                                     block:^(NSTimer* t) {
                                         (void)t;
                                         tick_foreground_grants();
                                     }];
        m_foreground_grant_timer = (__bridge_retained void*)timer;
    });
}

void CocoaBridge::end_foreground_grants() {
    m_foreground_grant_roots.clear();
    m_foreground_yielded.clear();
    if (m_foreground_grant_timer) {
        NSTimer* timer = (__bridge_transfer NSTimer*)m_foreground_grant_timer;
        m_foreground_grant_timer = nullptr;
        [timer invalidate];
    }
}

void CocoaBridge::tick_foreground_grants() {
    // The grant ends when this application stops being the foreground one
    // (the user went elsewhere, or a descendant took activation), and with
    // the launched process.
    if (![NSApp isActive]) {
        end_foreground_grants();
        return;
    }
    std::erase_if(m_foreground_grant_roots, [](i32 root) {
        return ::kill(root, 0) != 0 && errno == ESRCH;
    });
    if (m_foreground_grant_roots.empty()) {
        end_foreground_grants();
        return;
    }
    if (@available(macOS 14.0, *)) {
        const i32 self_pid = static_cast<i32>(::getpid());
        for (NSRunningApplication* app in
             [[NSWorkspace sharedWorkspace] runningApplications]) {
            const i32 pid = static_cast<i32>([app processIdentifier]);
            if (pid == self_pid ||
                std::find(m_foreground_yielded.begin(),
                          m_foreground_yielded.end(),
                          pid) != m_foreground_yielded.end()) {
                continue;
            }
            for (const i32 root : m_foreground_grant_roots) {
                if (pid_descends_from(pid, root, host_parent_pid)) {
                    [NSApp yieldActivationToApplication:app];
                    m_foreground_yielded.push_back(pid);
                    VITREO_TRACE("[cocoa] yielded activation to pid %d "
                                 "(tree %d)\n",
                                 static_cast<int>(pid),
                                 static_cast<int>(root));
                    break;
                }
            }
        }
    }
}

void CocoaBridge::restore_key_after_declined_activation(HWND declined, HWND stays_active) {
    vtr_run_on_main_async(^{
        // Hand key status back without reordering: the clicked window stays
        // where it is (above its owner), as on Windows.
        if (active_hwnd() == declined) set_active_hwnd(stays_active);
        m_preferred_key_hwnd.store(stays_active, std::memory_order_release);
        auto prev = m_windows.find(stays_active);
        if (stays_active != NULL_HWND && prev != m_windows.end() && prev->second.ns_window) {
            [(__bridge NSWindow*)prev->second.ns_window makeKeyWindow];
        }
        VITREO_TRACE("[cocoa] key window hwnd=0x%x declined by WM_MOUSEACTIVATE; "
                     "key back to 0x%x\n", declined, stays_active);
    });
}

void CocoaBridge::on_native_window_became_key(HWND hwnd) {
    // AppKit click-to-front on a borderless splash buries owned wizard
    // pages. Restore Win32 owner z-order before posting focus so
    // the guest sees the dialog that should stay on top.
    restack_owned_windows_above(hwnd);

    // restack may have made an owned dialog key without going through
    // set_active_hwnd yet — check the real NSApp key window. Do not also
    // gate on a stale m_active_hwnd: AppKit can deliver become-key for B
    // before resign-key for A, and refusing B leaves active stuck on A while
    // keys land on B (an address-bar edit's SetFocus/KillFocus thrash).
    {
        NSWindow* key = [NSApp keyWindow];
        if (key) {
            HWND key_hwnd = find_hwnd_for_window((__bridge void*)key);
            if (key_hwnd != NULL_HWND && key_hwnd != hwnd) {
                return;
            }
        }
    }

    // Pending-activation flushes and visibility sync can re-enter become-key
    // while this top-level is already active. Re-sending activation/focus
    // pairs with spurious resigns into a SetFocus/KillFocus thrash that drops
    // edit-caret focus on the next keypress.
    const HWND previous_active = active_hwnd();
    const bool already_active = previous_active == hwnd;
    set_active_hwnd(hwnd);
    m_preferred_key_hwnd.store(hwnd, std::memory_order_release);
    publish_dock_session(true);
    // Cancel a pending makeKey for a different top-level — visibility sync
    // or an earlier failed activation must not fight the real key window.
    {
        HWND pending =
            m_pending_activation_hwnd.load(std::memory_order_acquire);
        if (pending != NULL_HWND && pending != hwnd) {
            m_pending_activation_hwnd.compare_exchange_strong(
                pending, NULL_HWND, std::memory_order_acq_rel);
        }
    }
    if (!m_queue) return;
    // Gate on the APPLICATION-level latch alone, never on which HWND was last
    // active. exchange_app_active(true) returning false is exactly "we were
    // not application-active until now", which is the condition Win32
    // WM_ACTIVATEAPP describes.
    //
    // `already_active` is about the HWND, and active_hwnd() is set on
    // activation but never cleared on resign (on_app_did_resign_active only
    // refreshes the cursor clip). So after a round trip through another app it
    // still names this window, the && short-circuited, and NOTHING was sent --
    // not even the exchange, so the latch stayed stale as well.
    //
    // MEASURED: opening a folder activates Finder, the guest correctly
    // receives WM_ACTIVATEAPP(0), and clicking back on the app delivers
    // nothing. The guest keeps believing it is deactivated
    // and stops painting and handling input, which presents as a hung app
    // with a live window -- the main thread sitting in
    // MsgWaitForMultipleObjectsEx with no messages arriving, rather than any
    // deadlock.
    if (m_native_application_activation_callback) {
        // USER32 owns the whole Win32 activation transition, including the
        // WM_SETFOCUS that DefWindowProc performs for WM_ACTIVATE, and
        // ignores a become-key for the window that is already active.
        NSEvent* event = [NSApp currentEvent];
        u32 mouse_message = 0;
        if (event != nil) {
            if (event.type == NSEventTypeLeftMouseDown) mouse_message = WM::LBUTTONDOWN;
            else if (event.type == NSEventTypeRightMouseDown) mouse_message = WM::RBUTTONDOWN;
            else if (event.type == NSEventTypeOtherMouseDown) mouse_message = WM::MBUTTONDOWN;
        }
        const bool by_mouse = mouse_message != 0;
        (void)exchange_app_active(true);
        // Learn WM_MOUSEACTIVATE: a click in an inactive window asks it
        // first. MA_NOACTIVATE(ANDEAT) leaves the active window active --
        // VCL's floating menus answer that, and activating them deactivated
        // the frame, which closes the menu before the click selects an item.
        constexpr u32 MA_ACTIVATEANDEAT = 2, MA_NOACTIVATE = 3, MA_NOACTIVATEANDEAT = 4;
        const u32 answer = m_native_application_activation_callback(hwnd, true, mouse_message);
        if (answer == MA_ACTIVATEANDEAT || answer == MA_NOACTIVATEANDEAT) {
            m_mouse_activate_eat_hwnd.store(hwnd, std::memory_order_release);
        }
        if (answer == MA_NOACTIVATE || answer == MA_NOACTIVATEANDEAT) {
            restore_key_after_declined_activation(hwnd, previous_active);
            return;
        }
        constexpr u32 kDeferred = 0x10000;  // User32ExtManager::kMouseActivateDeferred
        if (answer == kDeferred) {
            VITREO_TRACE("[cocoa] key window hwnd=0x%x -> WM_MOUSEACTIVATE asked on "
                         "the window's thread\n", hwnd);
            return;
        }
        VITREO_TRACE("[cocoa] key window hwnd=0x%x -> USER32 activation "
                     "(by_mouse=%d)\n", hwnd, by_mouse ? 1 : 0);
        if (diag::focus_diag()) {
            std::fprintf(stderr,
                         "[focus] becameKey hwnd=0x%x already_active=%d\n",
                         hwnd, already_active ? 1 : 0);
        }
        return;
    }
    if (!exchange_app_active(true)) {
        MSG msg{};
        msg.hwnd = hwnd;
        msg.message = WM::ACTIVATEAPP;
        msg.wParam = 1;
        m_queue->post(msg);
        VITREO_TRACE(
            "[cocoa] key window hwnd=0x%x -> queued WM_ACTIVATEAPP(1) "
            "(no USER32 activation callback)\n",
            hwnd);
    }
    // WM_SETFOCUS is also a sent notification, distinct from queued keyboard
    // input. Route it through USER32 when fully wired; retain the queue
    // fallback for isolated bridge use. set_native_keyboard_focus no-ops when
    // a child of this top-level already owns focus.
    if (m_native_keyboard_focus_callback) {
        m_native_keyboard_focus_callback(hwnd, true);
        VITREO_TRACE("[cocoa] key window hwnd=0x%x -> sent WM_SETFOCUS\n",
                     hwnd);
    } else if (!already_active) {
        MSG focus{};
        focus.hwnd = hwnd;
        focus.message = WM::SETFOCUS;
        focus.wParam = 0;
        m_queue->post(focus);
        VITREO_TRACE(
            "[cocoa] key window hwnd=0x%x -> queued WM_SETFOCUS "
            "(no USER32 focus callback)\n",
            hwnd);
    }
    if (diag::focus_diag()) {
        std::fprintf(stderr,
                     "[focus] becameKey hwnd=0x%x already_active=%d\n",
                     hwnd, already_active ? 1 : 0);
    }
}

void CocoaBridge::on_native_window_resigned_key(HWND hwnd) {
    // AppKit emits resign/become pairs around makeKeyAndOrderFront and
    // first-responder churn even when this window remains the key window.
    // Treat those as no-ops: Windows does not WM_KILLFOCUS a top-level that
    // never actually lost activation.
    HWND key_hwnd = NULL_HWND;
    {
        auto it = m_windows.find(hwnd);
        if (it != m_windows.end() && it->second.ns_window) {
            NSWindow* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
            if ([win isKeyWindow]) {
                if (diag::focus_diag()) {
                    std::fprintf(stderr,
                                 "[focus] resignKey hwnd=0x%x ignored "
                                 "(still isKeyWindow)\n",
                                 hwnd);
                }
                return;
            }
        }
        NSWindow* key = [NSApp keyWindow];
        key_hwnd =
            key ? find_hwnd_for_window((__bridge void*)key) : NULL_HWND;
        if (key_hwnd == hwnd) {
            if (diag::focus_diag()) {
                std::fprintf(stderr,
                             "[focus] resignKey hwnd=0x%x ignored "
                             "(NSApp.keyWindow still self)\n",
                             hwnd);
            }
            return;
        }
        if (key_hwnd == NULL_HWND &&
            m_pending_activation_hwnd.load(std::memory_order_acquire) ==
                hwnd) {
            if (diag::focus_diag()) {
                std::fprintf(stderr,
                             "[focus] resignKey hwnd=0x%x ignored "
                             "(pending self activation)\n",
                             hwnd);
            }
            return;
        }
        // Stale resign after another top-level already claimed activation.
        // Applying it clears focus / posts ACTIVATEAPP(0) and re-opens the
        // null-active gap that lets sibling visibility sync steal key.
        const HWND recorded_active = active_hwnd();
        if (recorded_active != NULL_HWND && recorded_active != hwnd) {
            if (diag::focus_diag()) {
                std::fprintf(stderr,
                             "[focus] resignKey hwnd=0x%x ignored "
                             "(active=0x%x)\n",
                             hwnd, recorded_active);
            }
            return;
        }
    }

    clear_active_hwnd(hwnd);
    if (!m_queue) return;
    if (m_native_application_activation_callback) {
        // Key moving to another guest top-level is that window's activation,
        // which its become-key performs (deactivating this one first).
        // Otherwise the application lost activation: USER32 sends
        // WM_NCACTIVATE, WM_ACTIVATE(WA_INACTIVE), WM_ACTIVATEAPP(FALSE) and
        // WM_KILLFOCUS.
        if (key_hwnd == NULL_HWND) {
            (void)exchange_app_active(false);
            m_native_application_activation_callback(hwnd, false, false);
            VITREO_TRACE("[cocoa] resigned key window hwnd=0x%x -> USER32 "
                         "deactivation\n", hwnd);
        }
        return;
    }
    if (m_native_keyboard_focus_callback) {
        m_native_keyboard_focus_callback(hwnd, false);
        VITREO_TRACE(
            "[cocoa] resigned key window hwnd=0x%x -> sent WM_KILLFOCUS\n",
            hwnd);
    } else {
        MSG focus{};
        focus.hwnd = hwnd;
        focus.message = WM::KILLFOCUS;
        focus.wParam = 0;
        m_queue->post(focus);
        VITREO_TRACE(
            "[cocoa] resigned key window hwnd=0x%x -> queued WM_KILLFOCUS "
            "(no USER32 focus callback)\n",
            hwnd);
    }
    if (diag::focus_diag()) {
        std::fprintf(stderr, "[focus] resignKey hwnd=0x%x applied\n", hwnd);
    }

    // Transferring AppKit key to another guest top-level is not application
    // deactivation. Skip WM_ACTIVATEAPP(0) so the guest does
    // not tear down input state between two of its own HWNDs.
    if (key_hwnd != NULL_HWND) {
        if (diag::focus_diag()) {
            std::fprintf(stderr,
                         "[focus] resignKey hwnd=0x%x skip ACTIVATEAPP(0) "
                         "(key transferred to 0x%x)\n",
                         hwnd, key_hwnd);
        }
        return;
    }

    if (exchange_app_active(false)) {
        MSG msg{};
        msg.hwnd = hwnd;
        msg.message = WM::ACTIVATEAPP;
        msg.wParam = 0;
        m_queue->post(msg);
        VITREO_TRACE(
            "[cocoa] resigned key window hwnd=0x%x -> queued "
            "WM_ACTIVATEAPP(0) (no USER32 activation callback)\n",
            hwnd);
    }
}

void CocoaBridge::on_app_did_resign_active() {
    // A guest ClipCursor must stop confining the host cursor as soon as we
    // stop being the active application, whether or not any window is in
    // exclusive fullscreen (see refresh_cursor_clip_state).
    refresh_cursor_clip_state();
    // The display-wide CGDisplayHideCursor of an exclusive game is released
    // too: the app the user switched to owns the cursor now.
    apply_host_cursor_visibility();
    // Full Screen is a Space. Cmd-Tab leaves that Space; do not orderOut or
    // demote the window (that was the covering-display path). Only levels
    // raised above a full-screen sibling are dropped (host_window_level):
    // nothing of an inactive app may stay above the app now in front. The
    // visibility re-sync on activation raises them again.
    restore_elevated_window_levels();
}

void CocoaBridge::on_app_did_become_active() {
    // The start grant is used by the first activation.
    m_holds_start_grant.store(false, std::memory_order_release);
    m_foreground_refusal_flashed.store(false, std::memory_order_release);
    m_fullscreen_presentation_suspended = false;
    // Re-arm a guest ClipCursor that was released while we were inactive.
    refresh_cursor_clip_state();
    enforce_cursor_clip();
    sync_exclusive_space_system_ui();
    apply_host_cursor_visibility();
    // Dock click / Cmd-Tab activates an app whose HWNDs may still be
    // orderOut because the original ShowWindow ran without activation
    // permission. Re-apply Win32 visibility now that we are active.
    for (auto& [hwnd, state] : m_windows) {
        (void)hwnd;
        sync_native_window_visibility(state);
    }
    // A Space requested while the app could not take it is formed now.
    std::vector<HWND> wanted_spaces;
    for (const auto& [hwnd, state] : m_windows) {
        if (state.fullscreen) wanted_spaces.push_back(hwnd);
    }
    for (const HWND hwnd : wanted_spaces) reconcile_space_fullscreen(hwnd);
    // Taskbar restore analog: WM_SYSCOMMAND SC_RESTORE (0xF120). Learn
    // DefWindowProc carries that out as ShowWindow(SW_RESTORE).
    //
    // Only a *minimized* window is eligible. Windows never generates
    // SC_RESTORE because an application was activated, so posting it to
    // every orderOut HWND makes hidden helper windows appear. A
    // embedded host and a toolkit popup are created without
    // WS_VISIBLE and popped up as empty 640x480 windows on the first Dock
    // click. ShowWindow(SW_HIDE) is not a minimized state.
    if (m_queue != nullptr) {
        for (const auto& [hwnd, state] : m_windows) {
            if (state.ns_window == nullptr) continue;
            NSWindow* win = reinterpret_cast<NSWindow*>(state.ns_window);
            if (![win isMiniaturized] || hwnd != dock_session_target()) continue;
            MSG restore{};
            restore.hwnd = hwnd;
            restore.message = WM::SYSCOMMAND;
            restore.wParam = 0xF120; // SC_RESTORE
            m_queue->post(restore);
        }
    }
}

void CocoaBridge::on_native_occlusion_changed(HWND hwnd, bool visible) {
    std::lock_guard<std::mutex> lock(m_occlusion_mutex);
    const auto found = std::find(m_occluded_windows.begin(),
                                 m_occluded_windows.end(), hwnd);
    if (visible && found != m_occluded_windows.end()) {
        m_occluded_windows.erase(found);
    } else if (!visible && found == m_occluded_windows.end()) {
        m_occluded_windows.push_back(hwnd);
    }
    VITREO_TRACE("[cocoa] occlusion hwnd=0x%x visible=%d\n", hwnd,
                 visible ? 1 : 0);
}

bool CocoaBridge::window_occluded(HWND hwnd) const {
    std::lock_guard<std::mutex> lock(m_occlusion_mutex);
    return std::find(m_occluded_windows.begin(), m_occluded_windows.end(),
                     hwnd) != m_occluded_windows.end();
}

} // namespace vitreo::windowing
