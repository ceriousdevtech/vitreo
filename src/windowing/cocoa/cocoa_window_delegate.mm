// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_window_delegate.mm
/// @brief NSWindowDelegate callbacks turned into Win32 window notifications.
///
/// VtrWindowDelegate receives AppKit's notifications for one top-level
/// window -- close button, key changes, miniaturize, fullscreen
/// transitions, occlusion, move and live resize -- and reports each as the
/// WM_* message or CocoaBridge callback the Win32 side expects.
///
/// Split out of cocoa_bridge.mm, which was 7,624 lines. The class is
/// unchanged; only its translation unit is new.

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

// Minimum interval between resize-driven WM_SIZE posts (in seconds).
// ~60 fps throttle keeps the guest from being overwhelmed during live resize.
static constexpr CFAbsoluteTime kResizeThrottleInterval = 1.0 / 60.0;

// =============================================================================
// VtrWindowDelegate — handles close button → WM_CLOSE
// =============================================================================

@implementation VtrWindowDelegate

- (instancetype)init {
    if ((self = [super init])) {
        _lastPostedW = -1;
        _lastPostedH = -1;
    }
    return self;
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (_queue) {
        vitreo::windowing::MSG msg{};
        msg.hwnd    = _hwnd;
        msg.message = vitreo::windowing::WM::CLOSE;
        _queue->post(msg);
    }
    return NO; // Don't close yet — let guest handle WM_CLOSE → DestroyWindow
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification;
    if (_bridge) {
        _bridge->on_native_window_became_key(_hwnd);
    }
}

- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    if (_bridge) {
        _bridge->on_native_window_resigned_key(_hwnd);
    }
}

- (void)windowDidMiniaturize:(NSNotification *)notification {
    (void)notification;
    if (_bridge) {
        _bridge->on_native_show_state(_hwnd, true, false);
    }
}

- (void)windowDidDeminiaturize:(NSNotification *)notification {
    NSWindow* window = (NSWindow*)notification.object;
    if (_bridge) {
        _bridge->on_native_show_state(
            _hwnd, false, [window isZoomed] == YES);
    }
}

- (NSRect)windowWillUseStandardFrame:(NSWindow*)window
                        defaultFrame:(NSRect)newFrame {
    // Option-green / zoom: is work-area maximize, not size-to-content.
    NSScreen* screen = [window screen] ?: [NSScreen mainScreen];
    return screen ? [screen visibleFrame] : newFrame;
}

- (NSApplicationPresentationOptions)window:(NSWindow*)window
    willUseFullScreenPresentationOptions:
        (NSApplicationPresentationOptions)proposedOptions {
    (void)window;
    // Document Space: AppKit's AutoHide (menu bar / Dock reveal on edge).
    // Exclusive / CDS Space: Hide, not AutoHide — no edge reveal.
    if (_bridge && _bridge->exclusive_space_hides_system_ui(_hwnd)) {
        return NSApplicationPresentationFullScreen |
               NSApplicationPresentationHideDock |
               NSApplicationPresentationHideMenuBar;
    }
    return proposedOptions;
}

- (void)windowWillEnterFullScreen:(NSNotification*)notification {
    (void)notification;
    if (_bridge) {
        _bridge->on_native_space_transition_began(_hwnd);
    }
    // The delegate return value alone still AutoHides the Dock. Hide must
    // also be set on NSApp (Apple presentation-options override).
    if (_bridge && _bridge->exclusive_space_hides_system_ui(_hwnd)) {
        _bridge->apply_exclusive_space_system_ui(true);
    }
}

- (void)windowDidChangeOcclusionState:(NSNotification*)notification {
    NSWindow* window = (NSWindow*)notification.object;
    if (_bridge && window) {
        _bridge->on_native_occlusion_changed(
            _hwnd,
            ([window occlusionState] & NSWindowOcclusionStateVisible) != 0);
    }
}

- (void)windowWillExitFullScreen:(NSNotification*)notification {
    (void)notification;
    if (_bridge) {
        _bridge->on_native_space_transition_began(_hwnd);
    }
}

- (void)windowDidFailToEnterFullScreen:(NSWindow*)window {
    (void)window;
    if (_bridge) {
        _bridge->on_native_space_transition_failed(_hwnd);
    }
}

- (void)windowDidFailToExitFullScreen:(NSWindow*)window {
    (void)window;
    if (_bridge) {
        _bridge->on_native_space_transition_failed(_hwnd);
    }
}

- (void)windowDidEnterFullScreen:(NSNotification*)notification {
    (void)notification;
    if (_bridge) {
        _bridge->on_native_did_enter_space_fullscreen(_hwnd);
    }
}

- (void)windowDidExitFullScreen:(NSNotification*)notification {
    (void)notification;
    if (_bridge) {
        _bridge->on_native_did_exit_space_fullscreen(_hwnd);
    }
}

- (void)windowWillMove:(NSNotification *)notification {
    (void)notification;
    const NSEventType type = [[NSApp currentEvent] type];
    if (_bridge && !_bridge->resize_notify_suppressed() &&
        (type == NSEventTypeLeftMouseDown || type == NSEventTypeLeftMouseDragged))
        _bridge->on_native_move_size(_hwnd, true);
}

- (void)windowDidMove:(NSNotification *)notification {
    if (_bridge) {
        _bridge->on_native_move(_hwnd, (__bridge void*)notification.object);
    }
}

- (void)windowDidResize:(NSNotification *)notification {
    NSWindow* window = (NSWindow*)notification.object;
    NSRect contentRect = [window contentRectForFrameRect:[window frame]];
    int32_t cw = static_cast<int32_t>(contentRect.size.width);
    int32_t ch = static_cast<int32_t>(contentRect.size.height);

    // Record maximize/restore before geometry. On a native zoom transition the
    // HWND callback must preserve the old normal rect before resize replaces
    // the current dimensions with the maximized frame.
    if (_bridge) {
        _bridge->on_native_show_state(
            _hwnd, [window isMiniaturized] == YES, [window isZoomed] == YES);
    }

    // Update bridge + HwndManager state immediately so GetClientRect returns
    // the correct size even when we throttle WM_SIZE messages. Programmatic
    // guest SetWindowPos/MoveWindow is different: set_frame() already updated
    // the Win32 geometry and suppresses the synchronous AppKit callback so
    // Cocoa's title-bar/frame dimensions cannot feed back as a second guest
    // resize.
    // A left/top-edge resize changes the origin too. Publish that observed
    // origin before the size, regardless of AppKit notification ordering.
    if (_bridge) _bridge->on_native_move(_hwnd, (__bridge void*)window);
    const bool guest_notified = _bridge &&
        _bridge->on_native_resize(_hwnd, cw, ch);

    // Skip the WM_SIZE post when this resize was Vitreo's internal grow-to-fit
    // (not a user action): the guest's WM_SIZE handler would relayout controls
    // for a size change it never requested.
    if (_bridge && _bridge->resize_notify_suppressed()) {
        return;
    }

    // Suppress WM_SIZE when the client size has NOT actually changed. AppKit
    // re-fires windowDidResize whenever the guest's WM_SIZE handler touches the
    // window/its frame (e.g. SetWindowPos to lay out child controls); without
    // this guard a dialog's WM_SIZE → layout → resize-notify → identical WM_SIZE
    // loops forever and pinwheels the UI. A real drag changes cw/ch each event
    // so legitimate resizes still post.
    if (cw == _lastPostedW && ch == _lastPostedH) {
        return;
    }
    _lastPostedW = cw;
    _lastPostedH = ch;

    // Throttle WM_SIZE notifications during live resize to ~60fps so the
    // guest CPU interpreter is not overwhelmed with redundant relayouts.
    static CFAbsoluteTime s_last_resize_post = 0;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    bool is_live = [window inLiveResize];
    if (is_live && (now - s_last_resize_post) < kResizeThrottleInterval) {
        return; // skip this intermediate event
    }
    s_last_resize_post = now;

    // Track live-resize state so the guest message loop can avoid blocking
    // the main thread with pump_events / wait_events during the drag.
    if (is_live && _bridge) {
        _bridge->on_native_move_size(_hwnd, true);
        _bridge->set_live_resize(true);
    }

    VITREO_TRACE( "[cocoa] windowDidResize hwnd=0x%x client=%dx%d%s\n",
                 _hwnd, cw, ch, is_live ? " (live)" : "");

    // Track the peer's presentation view with the window BEFORE the guest is
    // told: the guest's response is to render at the new size, and it must
    // have somewhere correctly sized to render into.
    if (_bridge) {
        _bridge->resize_hosted_peer_children_for_window(
            (__bridge void*)[notification object]);
    }

    if (_queue && !guest_notified) {
        // Coalesce prior WM_SIZE notifications so the guest only processes
        // the latest size, avoiding a queued "card shuffle" animation after
        // the resize ends. WM_PAINT is not a posted message on Windows; the
        // user32 resize callback invalidates the update region and the guest
        // message loop generates WM_PAINT only after posted messages drain.
        _queue->remove_by_type(_hwnd, vitreo::windowing::WM::SIZE);

        // Post WM_SIZE with new client dimensions
        vitreo::windowing::MSG msg_size{};
        msg_size.hwnd    = _hwnd;
        msg_size.message = vitreo::windowing::WM::SIZE;
        msg_size.wParam  = 0; // SIZE_RESTORED
        msg_size.lParam  = static_cast<uint64_t>(
            (cw & 0xFFFF) | ((ch & 0xFFFF) << 16));
        _queue->post(msg_size);

    }

}

/// Set the live-resize flag as early as possible so the guest message
/// loop skips pump_events / wait_events from the very start of the drag.
- (void)windowWillStartLiveResize:(NSNotification *)notification {
    NSWindow* window = (NSWindow*)notification.object;
    VITREO_TRACE( "[cocoa] windowWillStartLiveResize hwnd=0x%x\n", _hwnd);
    if (_bridge) {
        _bridge->on_native_move_size(_hwnd, true);
        _bridge->set_live_resize(true);
    }

    // Start a 60 fps timer on NSRunLoopCommonModes.  Unlike
    // CFRunLoopPerformBlock, NSTimers ARE fired during AppKit's
    // event-tracking run-loop mode, so this is the reliable way to
    // push guest-rendered frames to the layer during a live resize.
    [_liveResizeTimer invalidate];
    _liveResizeTimer = [NSTimer timerWithTimeInterval:1.0/60.0
                                              target:self
                                            selector:@selector(liveResizeTimerFired:)
                                            userInfo:nil
                                             repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:_liveResizeTimer
                              forMode:NSRunLoopCommonModes];
}

/// Ensure the final size is always posted when live resize ends.
- (void)windowDidEndLiveResize:(NSNotification *)notification {
    NSWindow* window = (NSWindow*)notification.object;
    NSRect contentRect = [window contentRectForFrameRect:[window frame]];
    int32_t cw = static_cast<int32_t>(contentRect.size.width);
    int32_t ch = static_cast<int32_t>(contentRect.size.height);

    VITREO_TRACE( "[cocoa] windowDidEndLiveResize hwnd=0x%x client=%dx%d\n",
                 _hwnd, cw, ch);

    // Stop the live-resize blit timer.
    [_liveResizeTimer invalidate];
    _liveResizeTimer = nil;

    // Do one final blit of any pending pixels before switching back
    // to the normal sync path.
    bool guest_notified = false;
    if (_bridge) {
        _bridge->blit_pending_pixels(_hwnd);
        _bridge->set_live_resize(false);
        guest_notified = _bridge->on_native_resize(_hwnd, cw, ch);
    }

    if (_queue && !guest_notified) {
        // Remove stale size notifications and post the final size. The
        // on_native_resize callback above invalidates the update region;
        // user32 generates the resulting low-priority WM_PAINT.
        _queue->remove_by_type(_hwnd, vitreo::windowing::WM::SIZE);

        vitreo::windowing::MSG msg_size{};
        msg_size.hwnd    = _hwnd;
        msg_size.message = vitreo::windowing::WM::SIZE;
        msg_size.wParam  = 0;
        msg_size.lParam  = static_cast<uint64_t>(
            (cw & 0xFFFF) | ((ch & 0xFFFF) << 16));
        _queue->post(msg_size);

    }
    if (_bridge) _bridge->on_native_move_size(_hwnd, false);
}

- (void)liveResizeTimerFired:(NSTimer*)timer {
    (void)timer;
    if (_bridge) {
        _bridge->blit_pending_pixels(_hwnd);
    }
}

@end
