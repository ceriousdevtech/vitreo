// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_move_size.mm
/// @brief Native move and resize, and the callbacks that report them.
///
/// A window moved or resized by AppKit, or by a guest-drawn frame's
/// WM_NCHITTEST answer, is reported back to the Win32 side through these
/// callbacks; the guest-frame drag and resize loops are driven from here.
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

// =============================================================================
// Native Resize Callback
// =============================================================================

void CocoaBridge::set_native_resize_callback(NativeResizeCallback cb) {
    m_resize_callback = std::move(cb);
}

void CocoaBridge::on_native_move(HWND hwnd, void* ns_window) {
    // set_frame() (guest SetWindowPos/MoveWindow) already owns the geometry;
    // its synchronous windowDidMove must not feed back as a second guest move.
    if (resize_notify_suppressed() || ns_window == nullptr) return;
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end() || it->second.fullscreen ||
        it->second.borderless_fullscreen) {
        return;
    }
    NSWindow* win = (__bridge NSWindow*)ns_window;
    if ([win isMiniaturized]) return;
    // Inverse of set_frame(): Cocoa content rect -> Win32 client origin.
    const NSRect content = [win contentRectForFrameRect:[win frame]];
    const CGFloat scale = kWin32PixelScale;
    const CGFloat screen_h = vtr_win32_flip_height();
    const i32 client_x = static_cast<i32>(std::lround(content.origin.x / scale));
    const i32 client_y = static_cast<i32>(std::lround(
        (screen_h - content.origin.y - content.size.height) / scale));
    Rect& frame = it->second.frame;
    if (frame.left == client_x && frame.top == client_y) return;
    const i32 w = frame.right - frame.left;
    const i32 h = frame.bottom - frame.top;
    frame = Rect{client_x, client_y, client_x + w, client_y + h};
    if (m_move_callback) m_move_callback(hwnd, client_x, client_y);
}

bool CocoaBridge::on_native_resize(HWND hwnd, i32 client_w, i32 client_h) {
    // NSWindow::setFrame synchronously emits windowDidResize. When that frame
    // change came from guest SetWindowPos/MoveWindow, USER32 already owns the
    // requested geometry. Feeding the host result back through the native
    // callback can count Cocoa title chrome twice and trigger a guest relayout.
    if (resize_notify_suppressed()) {
        if (diag::input_diag()) {
            std::fprintf(stderr,
                "[input] on_native_resize SUPPRESSED hwnd=0x%x %dx%d\n",
                hwnd, client_w, client_h);
        }
        return true;
    }

    // Update bridge-side frame state
    auto it = m_windows.find(hwnd);
    // AppKit's Space animation must not replace the guest's requested
    // borderless size with intermediate native sizes.
    if (it != m_windows.end() && it->second.borderless_fullscreen) return true;
    if (it != m_windows.end()) {
        it->second.frame.right  = it->second.frame.left + client_w;
        it->second.frame.bottom = it->second.frame.top  + client_h;
    }

    // Notify the HwndManager so GetClientRect returns the new size
    if (m_resize_callback) {
        return m_resize_callback(hwnd, client_w, client_h);
    }
    return false;
}

void CocoaBridge::set_native_show_state_callback(NativeShowStateCallback cb) {
    m_show_state_callback = std::move(cb);
}

void CocoaBridge::set_native_hit_test_callback(NativeHitTestCallback cb) {
    m_hit_test_callback = std::move(cb);
}

bool CocoaBridge::native_chrome_mouse_event(void* ns_event, HWND hwnd) {
    NSEvent* event = (__bridge NSEvent*)ns_event;
    if (event == nil || hwnd == NULL_HWND) return false;
    const NSEventType type = [event type];
    const bool left = type == NSEventTypeLeftMouseDown || type == NSEventTypeLeftMouseUp ||
                      type == NSEventTypeLeftMouseDragged;
    const u32 button = left ? 1u : 2u;
    // A gesture already in progress on the chrome keeps its drags and its
    // release away from the client.
    if (m_native_chrome_press_hwnd == hwnd && m_native_chrome_press_buttons != 0) {
        if (type == NSEventTypeLeftMouseUp || type == NSEventTypeRightMouseUp) {
            m_native_chrome_press_buttons &= ~button;
            if (m_native_chrome_press_buttons == 0) m_native_chrome_press_hwnd = NULL_HWND;
            return true;
        }
        if (type == NSEventTypeLeftMouseDragged || type == NSEventTypeRightMouseDragged) return true;
    }
    if (type != NSEventTypeLeftMouseDown && type != NSEventTypeRightMouseDown) return false;
    NSWindow* window = [event window];
    if (window == nil || ([window styleMask] & NSWindowStyleMaskTitled) == 0) return false;
    if (([window styleMask] & NSWindowStyleMaskFullScreen) != 0) return false;
    if (m_mouse_capture_hwnd.load(std::memory_order_acquire) == hwnd) return false;
    if (NSPointInRect([event locationInWindow], [window contentLayoutRect])) return false;
    m_native_chrome_press_hwnd = hwnd;
    m_native_chrome_press_buttons |= button;
    if (diag::input_diag()) {
        std::fprintf(stderr, "[input] native chrome press hwnd=0x%x button=%u: non-client, not posted\n",
                     hwnd, button);
    }
    return true;
}

bool CocoaBridge::perform_guest_frame_mouse_down(void* ns_event, HWND hwnd,
                                                  i32 client_x,
                                                  i32 client_y) {
    // Host-only pumps (D3D Present, Wait*, interpreter event pump) must not
    // run guest wndprocs. Sync WM_NCHITTEST here would reenter mid-host-call.
    // Exclusive fullscreen sets client_drawn_frame via
    // WM_NCCALCSIZE and then dies on click with 0xC0000096 / callback
    // sentinel 0xDEADC0DE.
    if (in_host_input_pump()) {
        if (diag::input_diag()) {
            std::fprintf(stderr,
                         "[input] guest-frame drag skipped hwnd=0x%x "
                         "host_pump=1\n",
                         hwnd);
        }
        return false;
    }

    auto it = m_windows.find(hwnd);
    // Fullscreen has no guest caption to AppKit-drag; a window in (or
    // moving into or out of) a Space never moves.
    const bool in_space = it != m_windows.end() &&
        it->second.ns_window != nullptr &&
        (window_is_space_fullscreen(
             reinterpret_cast<NSWindow*>(it->second.ns_window)) ||
         it->second.space_transition);
    if (it == m_windows.end() || it->second.ns_window == nullptr ||
        !it->second.client_drawn_frame || !m_hit_test_callback ||
        it->second.fullscreen || it->second.borderless_fullscreen ||
        in_space) {
        if (diag::input_diag()) {
            std::fprintf(stderr,
                         "[input] guest-frame drag skipped hwnd=0x%x "
                         "known=%d nswindow=%d client_drawn=%d cb=%d fs=%d\n",
                         hwnd, it != m_windows.end() ? 1 : 0,
                         (it != m_windows.end() && it->second.ns_window) ? 1 : 0,
                         (it != m_windows.end() && it->second.client_drawn_frame)
                             ? 1 : 0,
                         m_hit_test_callback ? 1 : 0,
                         (it != m_windows.end() && it->second.fullscreen) ? 1 : 0);
        }
        return false;
    }

    // A guest-owned frame has client == window coordinates after
    // WM_NCCALCSIZE. Convert that point to the Win32 screen coordinates
    // required by WM_NCHITTEST using the HWND's authoritative outer origin.
    const i32 screen_x = it->second.frame.left + client_x;
    const i32 screen_y = it->second.frame.top + client_y;
    const i32 hit = m_hit_test_callback(hwnd, screen_x, screen_y);
    if (diag::input_diag()) {
        // Is the guest's idea of its own top edge the same as AppKit's?
        // A caption band measured at y=24..44 while the visible title row is
        // at the very top would mean this mapping is offset, and every drag
        // lands in the HTCLIENT strip above the caption.
        NSWindow* w = reinterpret_cast<NSWindow*>(it->second.ns_window);
        const NSRect wf = [w frame];
        const NSRect cf = [[w contentView] frame];
        const NSRect cs = [w convertRectToScreen:cf];
        std::fprintf(stderr,
            "[input] frame-check hwnd=0x%x guest_outer=(%d,%d %dx%d) "
            "nswindow=(%.0f,%.0f %.0fx%.0f) content_in_win=(%.0f,%.0f "
            "%.0fx%.0f) content_screen_top=%.0f style=0x%lx\n",
            hwnd, it->second.frame.left, it->second.frame.top,
            it->second.frame.right - it->second.frame.left,
            it->second.frame.bottom - it->second.frame.top,
            wf.origin.x, wf.origin.y, wf.size.width, wf.size.height,
            cf.origin.x, cf.origin.y, cf.size.width, cf.size.height,
            cs.origin.y + cs.size.height,
            (unsigned long)[w styleMask]);
    }
    if (diag::input_diag()) {
        std::fprintf(stderr,
                     "[input] WM_NCHITTEST hwnd=0x%x screen=(%d,%d) "
                     "client=(%d,%d) -> %d\n",
                     hwnd, screen_x, screen_y, client_x, client_y, hit);
    }

    if (!guest_frame_drag_allowed(hit, it->second.client_drawn_frame,
                                  it->second.fullscreen ||
                                      it->second.borderless_fullscreen,
                                  in_space)) {
        // One-shot probe: ask the guest where (if anywhere) in its own top
        // strip it reports a caption. A guest-painted frame that never answers
        // HTCAPTION cannot be dragged by AppKit, and this says whether the
        // region is empty or merely somewhere else.
        static bool swept = false;
        if (!swept && diag::input_diag()) {
            swept = true;
            const i32 w = it->second.frame.right - it->second.frame.left;
            for (i32 py = 0; py < 48; py += 4) {
                std::string row;
                for (i32 px = 0; px < w; px += 32) {
                    const i32 h = m_hit_test_callback(
                        hwnd, it->second.frame.left + px,
                        it->second.frame.top + py);
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "%d ", h);
                    row += buf;
                }
                std::fprintf(stderr, "[input] ncsweep y=%2d: %s\n", py,
                             row.c_str());
            }
        }
        return false;
    }
    NSWindow* window = reinterpret_cast<NSWindow*>(it->second.ns_window);
    NSEvent* event = (__bridge NSEvent*)ns_event;
    on_native_move_size(hwnd, true);
    [window performWindowDragWithEvent:event];
    on_native_move_size(hwnd, false);
    return true;
}

namespace {

/// Live state of one interactive resize. Only the main thread touches it.
struct NativeResizeSession {
    void*   monitor = nullptr;      ///< id from addLocalMonitorForEvents...
    CocoaBridge* owner = nullptr;
    HWND hwnd = NULL_HWND;
    NSRect  anchor_frame{};         ///< window frame when the drag began
    NSPoint anchor_mouse{};         ///< screen mouse location when it began
    u32     edge = 0;               ///< HT::LEFT .. HT::BOTTOMRIGHT
};

NativeResizeSession g_resize;

/// Smallest window we will size down to, in AppKit points. AppKit happily
/// accepts a zero or negative frame and the result is an unrecoverable
/// window, so clamp rather than trusting the delta.
constexpr CGFloat kMinResizePoints = 80.0;

} // namespace

void CocoaBridge::on_native_move_size(HWND hwnd, bool entering) {
    if (entering) {
        if (!m_active_move_sizes.insert(hwnd).second) return;
    } else if (m_active_move_sizes.erase(hwnd) == 0) return;
    if (m_move_size_callback) m_move_size_callback(hwnd, entering);
}

void CocoaBridge::finish_native_move_size_for_window(void* ns_window) {
    on_native_move_size(find_hwnd_for_window(ns_window), false);
}

void CocoaBridge::finish_native_window_resize(HWND hwnd, bool notify) {
    if (g_resize.owner != this || (hwnd != NULL_HWND && hwnd != g_resize.hwnd)) return;
    const HWND active = g_resize.hwnd;
    if (g_resize.monitor) [NSEvent removeMonitor:(id)g_resize.monitor];
    g_resize = {};
    m_owns_native_resize.store(false, std::memory_order_release);
    if (notify) on_native_move_size(active, false);
}

bool CocoaBridge::begin_native_window_resize(HWND hwnd, u32 edge) {
    // HT sizing codes, mirrored from def_window_proc.hpp. The windowing layer
    // must not include a winapi header, and duplicating four comparisons is
    // cheaper than a shared header for two constants.
    constexpr u32 kHtLeft = 10, kHtRight = 11, kHtTop = 12, kHtTopLeft = 13,
                  kHtTopRight = 14, kHtBottom = 15, kHtBottomLeft = 16,
                  kHtBottomRight = 17;
    if (edge < kHtLeft || edge > kHtBottomRight) return false;

    __block bool started = false;
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || it->second.ns_window == nullptr) return;
        NSWindow* window = reinterpret_cast<NSWindow*>(it->second.ns_window);

        // A second WM_NCLBUTTONDOWN before the first gesture ended (a guest
        // that re-sends on every mouse-move) must not stack monitors.
        if (g_resize.owner) g_resize.owner->finish_native_window_resize(NULL_HWND);
        g_resize.owner = this;
        g_resize.hwnd = hwnd;

        g_resize.edge = edge;
        g_resize.anchor_frame = [window frame];
        g_resize.anchor_mouse = [NSEvent mouseLocation];

        // A LOCAL MONITOR, deliberately, not a nextEventMatchingMask: loop.
        // The monitor is a callback on AppKit's normal dispatch: it neither
        // dequeues the shared event queue nor blocks the main thread, which
        // is what made the previous sizing implementation kill clicks
        // OS-wide (see appkit_modal_loop_steals_events). Returning nil
        // swallows the event so the guest sees no mouse traffic mid-gesture.
        const NSEventMask mask = NSEventMaskLeftMouseDragged |
                                 NSEventMaskLeftMouseUp;
        id monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                     handler:^NSEvent*(NSEvent* ev) {
            if ([ev type] == NSEventTypeLeftMouseUp) {
                finish_native_window_resize(hwnd);
                return nil;
            }

            const NSPoint now = [NSEvent mouseLocation];
            const CGFloat dx = now.x - g_resize.anchor_mouse.x;
            const CGFloat dy = now.y - g_resize.anchor_mouse.y;
            NSRect f = g_resize.anchor_frame;
            const u32 e = g_resize.edge;

            // AppKit's origin is BOTTOM-left and y grows upward, so a Win32
            // "top" edge moves the frame's height only, and a "bottom" edge
            // moves the origin as well. Getting this backwards is the classic
            // way a resize appears to drag the window instead of sizing it.
            const bool west = (e == kHtLeft || e == kHtTopLeft ||
                               e == kHtBottomLeft);
            const bool east = (e == kHtRight || e == kHtTopRight ||
                               e == kHtBottomRight);
            const bool north = (e == kHtTop || e == kHtTopLeft ||
                                e == kHtTopRight);
            const bool south = (e == kHtBottom || e == kHtBottomLeft ||
                                e == kHtBottomRight);

            if (west) { f.origin.x += dx; f.size.width  -= dx; }
            if (east) {                   f.size.width  += dx; }
            if (south){ f.origin.y += dy; f.size.height -= dy; }
            if (north){                   f.size.height += dy; }

            // Clamp against the minimum, keeping the OPPOSITE edge pinned:
            // shrinking past the limit from the west must stop the west edge,
            // not start walking the east one.
            if (f.size.width < kMinResizePoints) {
                if (west) {
                    f.origin.x = NSMaxX(g_resize.anchor_frame) -
                                 kMinResizePoints;
                }
                f.size.width = kMinResizePoints;
            }
            if (f.size.height < kMinResizePoints) {
                if (south) {
                    f.origin.y = NSMaxY(g_resize.anchor_frame) -
                                 kMinResizePoints;
                }
                f.size.height = kMinResizePoints;
            }

            [window setFrame:f display:YES];
            return nil;
        }];
        g_resize.monitor = (void*)monitor;
        if (monitor) {
            m_owns_native_resize.store(true, std::memory_order_release);
            on_native_move_size(hwnd, true);
            started = true;
        } else {
            g_resize = {};
        }
    });

    if (diag::input_diag()) {
        std::fprintf(stderr,
                     "[input] begin_native_window_resize hwnd=0x%x edge=%u -> %d\n",
                     hwnd, edge, started ? 1 : 0);
    }
    return started;
}

bool CocoaBridge::begin_native_window_drag(HWND hwnd) {
    __block bool started = false;
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || it->second.ns_window == nullptr) return;
        NSEvent* event = [NSApp currentEvent];
        if (event == nil) return;
        NSWindow* window = reinterpret_cast<NSWindow*>(it->second.ns_window);
        // WM_NCLBUTTONDOWN(HTCAPTION) / SC_MOVE on a window that is in a
        // Space or exclusive full-screen: there is nothing to move (policy
        // rule 3; the guest's exclusive window keeps the display).
        if (it->second.fullscreen || it->second.borderless_fullscreen ||
            it->second.space_transition ||
            window_is_space_fullscreen(window)) {
            return;
        }
        started = true;
        // performWindowDragWithEvent: runs its own mouse-tracking loop until
        // the button is released. The guest thread is inside SendMessage, so
        // run it asynchronously rather than holding that thread (and the main
        // thread) for the whole gesture.
        VtrWindowDelegate* delegate = (VtrWindowDelegate*)[window delegate];
        dispatch_async(dispatch_get_main_queue(), ^{
            // The delegate is retained by this block; teardown clears its
            // bridge pointer. Do not capture a possibly destroyed bridge.
            if (delegate.bridge) delegate.bridge->on_native_move_size(hwnd, true);
            [window performWindowDragWithEvent:event];
            if (delegate.bridge) delegate.bridge->on_native_move_size(hwnd, false);
        });
    });
    if (diag::input_diag()) {
        std::fprintf(stderr, "[input] begin_native_window_drag hwnd=0x%x -> %d\n",
                     hwnd, started ? 1 : 0);
    }
    return started;
}

void CocoaBridge::on_native_show_state(HWND hwnd, bool minimized,
                                       bool maximized) {
    // A guest-owned setFrame can make AppKit transiently report isZoomed while
    // windowDidResize is delivered. Do not convert that host-side geometry
    // artifact into WS_MAXIMIZE; the guest already owns the show state.
    if (resize_notify_suppressed()) {
        return;
    }
    if (m_show_state_callback) {
        m_show_state_callback(hwnd, minimized, maximized);
    }
    // Minimising the confining window releases a guest ClipCursor; restoring
    // it re-arms one that is still installed.
    refresh_cursor_clip_state();
    if (!minimized) enforce_cursor_clip();
}

} // namespace vitreo::windowing
