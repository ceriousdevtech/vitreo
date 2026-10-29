// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_window_style.mm
/// @brief Win32 window styles mapped to AppKit chrome, collection behaviour and Spaces.
///
/// WS_* and WS_EX_* bits become an NSWindow style mask, a collection
/// behaviour and a presentation recipe; fullscreen requests become a
/// Space or a borderless window. The Space transition callbacks and the
/// exclusive-fullscreen system UI live here too.
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

void CocoaBridge::on_native_space_transition_began(HWND hwnd) {
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) return;
    it->second.space_transition = true;
}

void CocoaBridge::on_native_space_transition_failed(HWND hwnd) {
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) return;
    it->second.space_transition = false;
    it->second.space_toggle_ours = false;
    VITREO_TRACE("[cocoa] space transition failed hwnd=0x%x\n", hwnd);
}

void CocoaBridge::reconcile_space_fullscreen(HWND hwnd) {
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end() || !it->second.ns_window) return;
    auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
    const SpaceAction action = space_reconcile_action(
        it->second.fullscreen, window_is_space_fullscreen(win),
        it->second.space_transition, [win isVisible] == YES,
        [NSApp isActive] == YES);
    if (action == SpaceAction::None) return;
    // windowWillEnter/ExitFullScreen marks the transition when AppKit takes
    // the toggle; a toggle it refuses leaves nothing pending, so a later
    // reconcile point (key window, activation, show) asks again.
    it->second.space_toggle_ours = true;
    VITREO_TRACE("[cocoa] space reconcile hwnd=0x%x -> %s\n", hwnd,
                 action == SpaceAction::Enter ? "enter" : "exit");
    [win toggleFullScreen:nil];
}

void CocoaBridge::on_native_did_enter_space_fullscreen(HWND hwnd) {
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end() || !it->second.ns_window) return;
    const bool ours = it->second.space_toggle_ours;
    it->second.space_transition = false;
    it->second.space_toggle_ours = false;
    if (ours && !it->second.fullscreen) {
        // Not wanted any more by the time the Space formed.
        reconcile_space_fullscreen(hwnd);
        return;
    }
    auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
    NSRect fr = [win frame];
    it->second.fullscreen_screen_frame = Rect{
        static_cast<i32>(fr.origin.x),
        static_cast<i32>(fr.origin.y),
        static_cast<i32>(fr.origin.x + fr.size.width),
        static_cast<i32>(fr.origin.y + fr.size.height)};
    VITREO_TRACE("[cocoa] space fullscreen entered hwnd=0x%x "
                 "%.0fx%.0f%+.0f%+.0f\n",
                 hwnd, fr.size.width, fr.size.height,
                 fr.origin.x, fr.origin.y);
    sync_exclusive_space_system_ui();
    apply_host_cursor_visibility();
}

void CocoaBridge::on_native_did_exit_space_fullscreen(HWND hwnd) {
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end() || !it->second.ns_window) return;
    const bool ours = it->second.space_toggle_ours;
    it->second.space_transition = false;
    it->second.space_toggle_ours = false;
    auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
    if (!it->second.fullscreen && it->second.saved_style_mask != 0) {
        [win setStyleMask:static_cast<NSUInteger>(it->second.saved_style_mask)];
    }
    VITREO_TRACE("[cocoa] space fullscreen exited hwnd=0x%x exclusive=%d\n",
                 hwnd, it->second.fullscreen ? 1 : 0);
    sync_exclusive_space_system_ui();
    apply_host_cursor_visibility();
    // An exit this bridge asked for while an exclusive request arrived
    // (the borderless rule leaving its Space as the guest switched modes)
    // ends with the Space wanted again: enter it now. A user's own exit is
    // left alone.
    if (ours) reconcile_space_fullscreen(hwnd);
}

// =============================================================================
// Style Mapping
// =============================================================================

bool CocoaBridge::macos_session_is_child_process() noexcept {
    const char* process_role = std::getenv("VITREO_MACOS_PROCESS_ROLE");
    return process_role != nullptr && std::strcmp(process_role, "child") == 0;
}

u64 CocoaBridge::guest_chrome_style_mask(u32 ws_style) noexcept {
    // The guest paints its own caption, so AppKit draws no buttons; the
    // window still has to be miniaturizable, because AppKit ignores
    // -miniaturize: on a window without NSWindowStyleMaskMiniaturizable
    // (measured: borderless and resizable-only windows stay on screen), and
    // the guest's own minimize button reaches the host as SW_MINIMIZE /
    // SC_MINIMIZE. The bit adds nothing visible to a window with no title bar.
    return ((ws_style & WS::THICKFRAME) != 0 ? NSWindowStyleMask::Resizable
                                             : NSWindowStyleMask::Borderless) |
           NSWindowStyleMask::Miniaturizable;
}

u64 CocoaBridge::map_style_to_cocoa(u32 ws_style) noexcept {
    if ((ws_style & WS::POPUP) && (ws_style & WS::CAPTION) != WS::CAPTION) {
        return NSWindowStyleMask::Borderless;
    }

    u64 mask = NSWindowStyleMask::Borderless;

    // WS_CAPTION is WS_BORDER|WS_DLGFRAME — require both bits, matching
    // compute_nc_insets / AdjustWindowRect. Testing `style & CAPTION` alone
    // treated border-only windows as titled and skewed client vs frame size.
    if ((ws_style & WS::CAPTION) == WS::CAPTION) {
        mask |= NSWindowStyleMask::Titled;
    }
    if (ws_style & WS::SYSMENU) {
        mask |= NSWindowStyleMask::Closable;
    }
    if (ws_style & WS::MINIMIZEBOX) {
        mask |= NSWindowStyleMask::Miniaturizable;
    }
    if (ws_style & WS::THICKFRAME) {
        mask |= NSWindowStyleMask::Resizable;
    }
    if (ws_style & WS::MAXIMIZEBOX) {
        mask |= NSWindowStyleMask::Resizable;
    }

    return mask;
}

bool CocoaBridge::allows_space_fullscreen(u32 ws_style, u32 ex_style,
                                          bool is_dialog,
                                          bool client_drawn_frame) noexcept {
    PresentationSignals s{};
    s.style = ws_style;
    s.ex_style = ex_style;
    s.is_dialog = is_dialog;
    s.client_drawn_frame = client_drawn_frame;
    return make_presentation_recipe(s).space_fullscreen;
}

bool CocoaBridge::uses_space_fullscreen_collection(
    bool exclusive, u32 ws_style, u32 ex_style, bool is_dialog,
    bool client_drawn_frame) noexcept {
    PresentationSignals s{};
    s.style = ws_style;
    s.ex_style = ex_style;
    s.is_dialog = is_dialog;
    s.client_drawn_frame = client_drawn_frame;
    s.exclusive_fullscreen = exclusive;
    return make_presentation_recipe(s).space_fullscreen;
}

bool CocoaBridge::exclusive_space_hides_system_ui(bool exclusive) noexcept {
    PresentationSignals s{};
    s.exclusive_fullscreen = exclusive;
    return make_presentation_recipe(s).hide_system_ui;
}

bool CocoaBridge::exclusive_space_hides_system_ui(HWND hwnd) const {
    auto it = m_windows.find(hwnd);
    return it != m_windows.end() && it->second.fullscreen;
}

bool CocoaBridge::host_cursor_should_hide(bool guest_hidden, bool exclusive,
                                          bool has_installed_os_cursor,
                                          bool app_active) noexcept {
    (void)has_installed_os_cursor;
    return guest_hidden && exclusive && app_active;
}

void CocoaBridge::apply_exclusive_space_system_ui(bool hide) {
    @try {
        if (hide) {
            // Hide, not AutoHide. Do not include FullScreen here: AppKit
            // already owns the Space, and FullScreen+Hide still edge-reveals
            // the Dock on current macOS.
            [NSApp setPresentationOptions:
                NSApplicationPresentationHideDock |
                NSApplicationPresentationHideMenuBar];
        } else {
            [NSApp setPresentationOptions:NSApplicationPresentationDefault];
        }
    } @catch (NSException* ex) {
        VITREO_TRACE("[cocoa] setPresentationOptions failed: %s\n",
                     ex.reason ? [ex.reason UTF8String] : "");
    }
    apply_host_cursor_visibility();
}

void CocoaBridge::sync_exclusive_space_system_ui() {
    bool hide = false;
    for (const auto& [hwnd, state] : m_windows) {
        (void)hwnd;
        if (state.fullscreen) {
            hide = true;
            break;
        }
    }
    apply_exclusive_space_system_ui(hide);
}

void apply_presentation_recipe(NSWindow* win, const PresentationRecipe& recipe) {
    if (!win) return;
    [win setHasShadow:recipe.shadow ? YES : NO];
    // WS_EX_TOPMOST floats -- except for a window in a Space, which is alone
    // there and must not stay above the app the user switches to
    // (CocoaBridge::host_window_level).
    if (recipe.window_level == PresentationLevel::Elevated &&
        !window_is_space_fullscreen(win)) {
        [win setLevel:NSFloatingWindowLevel];
    }
    if ([win isKindOfClass:[VtrWindow class]]) {
        [(VtrWindow*)win setAllowKey:recipe.activating ? YES : NO];
    } else if ([win isKindOfClass:[VtrPanel class]]) {
        [(VtrPanel*)win setAllowKey:recipe.activating ? YES : NO];
    }

    NSWindowCollectionBehavior behavior = [win collectionBehavior];
    behavior &= ~(NSWindowCollectionBehaviorFullScreenPrimary |
                  NSWindowCollectionBehaviorFullScreenNone |
                  NSWindowCollectionBehaviorFullScreenAuxiliary |
                  NSWindowCollectionBehaviorTransient |
                  NSWindowCollectionBehaviorIgnoresCycle);
    if (recipe.space_fullscreen) {
        behavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    } else if (!recipe.dock_tile) {
        behavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
    } else {
        behavior |= NSWindowCollectionBehaviorFullScreenNone;
    }
    if (!recipe.dock_tile) {
        behavior |= NSWindowCollectionBehaviorTransient |
                    NSWindowCollectionBehaviorIgnoresCycle;
    }
    [win setCollectionBehavior:behavior];
}

bool CocoaBridge::composition_host_should_be_clear(u32 ws_style, u32 ex_style,
                                                   i32 client_w, i32 client_h,
                                                   i32 screen_w,
                                                   i32 screen_h) noexcept {
    constexpr u32 kWsExLayered = 0x00080000u;
    if ((ex_style & kWsExLayered) != 0) {
        return true;
    }
    (void)ws_style;
    (void)client_w;
    (void)client_h;
    (void)screen_w;
    (void)screen_h;
    // Covering WS_POPUP used to be treated as a skinned wizard host. Those
    // hosts are orderOut once they own dialogs; applying the same clear plate
    // to a display-covering game popup made the window-server skip it.
    return false;
}

void CocoaBridge::set_client_drawn_frame(HWND hwnd, bool client_drawn) {
    auto it = m_windows.find(hwnd);
    if (diag::input_diag()) {
        std::fprintf(stderr,
                     "[input] set_client_drawn_frame hwnd=0x%x -> %d "
                     "(known=%d, was=%d)\n",
                     hwnd, client_drawn ? 1 : 0,
                     it != m_windows.end() ? 1 : 0,
                     (it != m_windows.end() && it->second.client_drawn_frame)
                         ? 1 : 0);
    }
    if (it == m_windows.end() || it->second.client_drawn_frame == client_drawn) {
        return;
    }
    it->second.client_drawn_frame = client_drawn;
    const u32 guest_style = it->second.guest_style;
    vtr_run_on_main_sync(^{
        if (!it->second.ns_window) return;
        auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        const auto rec = make_presentation_recipe(make_presentation_signals(
            guest_style, it->second.guest_ex_style, it->second.is_dialog,
            client_drawn, it->second.owner != NULL_HWND, false,
            it->second.fullscreen));
        NSUInteger mask = static_cast<NSUInteger>(
            rec.chrome == PresentationChrome::Native
                ? map_style_to_cocoa(guest_style)
                : guest_chrome_style_mask(guest_style));
        // AppKit owns NSWindowStyleMaskFullScreen: it sets and clears that bit
        // only as part of a fullscreen transition, and clearing it from a
        // plain -setStyleMask: raises NSGenericException ("cleared on a window
        // outside of a full screen transition"). That is an uncaught
        // Objective-C exception, so it does not degrade -- it aborts the
        // process. Every mask above is derived from guest style bits and can
        // never carry the bit, so a guest that toggles its non-client owner
        // while in a Space (the guest does, on entering its world) would kill the
        // run. Carry the window's current bit across.
        //
        // on_native_did_exit_space_fullscreen's setStyleMask does not need
        // this: it runs inside the transition, where clearing is legal.
        if (window_is_space_fullscreen(win)) {
            mask |= static_cast<NSUInteger>(NSWindowStyleMaskFullScreen);
        }
        set_suppress_resize_notify(true);
        [win setStyleMask:mask];
        set_suppress_resize_notify(false);
        apply_composition_host_appearance(
            win, rec.clear_host,
            unpainted_plate_color(it->second.is_dialog, it->second.erase_color));
        apply_guest_click_through(win, it->second.guest_ex_style);
        if (guest_window_is_click_through_overlay(it->second.guest_style,
                                                  it->second.guest_ex_style)) {
            apply_overlay_appearance(win);
        }
        apply_presentation_recipe(win, rec);
        if (it->second.layered_alpha) apply_layered_alpha_appearance(win);
        sync_borderless_fullscreen(hwnd);
        VITREO_TRACE("[cocoa] hwnd=0x%x non-client owner=%s styleMask=0x%llx\n",
                     hwnd, client_drawn ? "guest" : "native",
                     static_cast<unsigned long long>(mask));
    });
}

void CocoaBridge::set_window_fullscreen(HWND hwnd, bool enable) {
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || !it->second.ns_window) return;
        auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);

        if (enable) {
            if (it->second.fullscreen) {
                // An exclusive owner (DXGI / D3D9 / DirectDraw / CDS) asking
                // for a Space the borderless-fullscreen rule already holds
                // takes it over: the window keeps its Space while the guest
                // mode change resizes it away from the monitor rectangle
                // (which would otherwise make the borderless rule leave it).
                if (it->second.borderless_fullscreen) {
                    it->second.borderless_fullscreen = false;
                    apply_space_fullscreen_collection_behavior(
                        win, true, it->second.guest_style,
                        it->second.guest_ex_style, it->second.is_dialog,
                        it->second.client_drawn_frame);
                    apply_exclusive_space_system_ui(true);
                    m_exclusive_cursor_confine_hwnd = hwnd;
                    refresh_cursor_clip_state();
                }
                // Idempotent for the guest; the host Space is re-checked in
                // case an earlier request was not taken by AppKit.
                reconcile_space_fullscreen(hwnd);
                return;
            }

            NSRect cur = [win frame];
            it->second.saved_frame = Rect{
                static_cast<i32>(cur.origin.x),
                static_cast<i32>(cur.origin.y),
                static_cast<i32>(cur.origin.x + cur.size.width),
                static_cast<i32>(cur.origin.y + cur.size.height)};
            it->second.saved_style_mask = static_cast<u64>([win styleMask]);

            NSScreen* screen = [win screen] ?: [NSScreen mainScreen];
            NSRect screenFrame = screen ? [screen frame] : NSMakeRect(0, 0, 1280, 720);
            it->second.fullscreen_screen_frame = Rect{
                static_cast<i32>(screenFrame.origin.x),
                static_cast<i32>(screenFrame.origin.y),
                static_cast<i32>(screenFrame.origin.x + screenFrame.size.width),
                static_cast<i32>(screenFrame.origin.y + screenFrame.size.height)};

            // Host presentation is a Space. Guest exclusive contracts
            // (back-buffer, clip, aspect-fit) stay on state.fullscreen.
            apply_space_fullscreen_collection_behavior(
                win, true, it->second.guest_style, it->second.guest_ex_style,
                it->second.is_dialog, it->second.client_drawn_frame);
            if (([win styleMask] & NSWindowStyleMaskResizable) == 0) {
                [win setStyleMask:[win styleMask] | NSWindowStyleMaskResizable];
            }
            // The Space is the full-screen host; a raised level (topmost)
            // would keep the window above other apps after Cmd-Tab.
            if ([win level] != NSNormalWindowLevel) {
                [win setLevel:NSNormalWindowLevel];
            }
            // Activating is the foreground verb: only a foreground app (or
            // one started by it) takes it; otherwise the Space forms when
            // the app is next activated (on_app_did_become_active).
            if (foreground_request_allowed(
                    [NSApp isActive] == YES,
                    m_holds_start_grant.load(std::memory_order_acquire))) {
                [NSApp activateIgnoringOtherApps:YES];
                [win makeKeyAndOrderFront:nil];
            }
            // Set before toggleFullScreen: so willUseFullScreenPresentationOptions
            // sees exclusive Hide (not document AutoHide).
            it->second.fullscreen = true;
            // A request that arrives during another transition (the
            // borderless rule leaving its Space) or before AppKit can take it
            // is applied when that becomes possible, not dropped.
            reconcile_space_fullscreen(hwnd);
            apply_exclusive_space_system_ui(true);
            m_exclusive_cursor_confine_hwnd = hwnd;
            refresh_cursor_clip_state();
            enforce_cursor_clip();
            VITREO_TRACE(
                "[cocoa] set_window_fullscreen hwnd=0x%x ON space "
                "%.0fx%.0f%+.0f%+.0f\n",
                hwnd, screenFrame.size.width, screenFrame.size.height,
                screenFrame.origin.x, screenFrame.origin.y);
        } else {
            if (!it->second.fullscreen) return;

            it->second.fullscreen = false;
            it->second.borderless_fullscreen = false;
            restore_elevated_window_levels();
            reconcile_space_fullscreen(hwnd);
            apply_space_fullscreen_collection_behavior(
                win, false, it->second.guest_style, it->second.guest_ex_style,
                it->second.is_dialog, it->second.client_drawn_frame);
            if (m_exclusive_cursor_confine_hwnd == hwnd) {
                m_exclusive_cursor_confine_hwnd = NULL_HWND;
            }

            bool any_other_fs = false;
            for (const auto& [other_hwnd, other] : m_windows) {
                if (other_hwnd != hwnd && other.fullscreen) {
                    any_other_fs = true;
                    if (m_exclusive_cursor_confine_hwnd == NULL_HWND) {
                        m_exclusive_cursor_confine_hwnd = other_hwnd;
                    }
                    break;
                }
            }
            if (!any_other_fs) m_fullscreen_presentation_suspended = false;
            sync_exclusive_space_system_ui();
            refresh_cursor_clip_state();
            if ([NSApp isActive]) {
                [win makeKeyAndOrderFront:nil];
            }

            VITREO_TRACE("[cocoa] set_window_fullscreen hwnd=0x%x OFF space\n",
                         hwnd);
        }
    });
    reapply_installed_cursor();
}

void CocoaBridge::sync_borderless_fullscreen(HWND hwnd) {
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end() || !it->second.ns_window) return;
    auto& state = it->second;
    if (state.fullscreen && !state.borderless_fullscreen) return;
    NSWindow* window = reinterpret_cast<NSWindow*>(state.ns_window);
    bool has_gpu_surface = false;
    for (NSView* view in [[window contentView] subviews]) {
        has_gpu_surface |= [view isKindOfClass:[VtrMetalView class]];
    }
    const auto signals = make_presentation_signals(
        state.guest_style, state.guest_ex_style, state.is_dialog,
        state.client_drawn_frame, state.owner != NULL_HWND,
        composition_host_defers_to_owned(state), false);
    bool enable = false;
    if (state.visible && !state.empty_window_rgn) {
        for (const auto& monitor : get_all_monitor_info()) {
            enable |= presentation_is_borderless_fullscreen(
                signals, has_gpu_surface, state.frame,
                Rect{monitor.left, monitor.top, monitor.right, monitor.bottom});
        }
    }
    if (enable == state.borderless_fullscreen) return;
    state.borderless_fullscreen = enable;
    set_window_fullscreen(hwnd, enable);
}

} // namespace vitreo::windowing
