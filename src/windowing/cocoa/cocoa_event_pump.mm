// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_event_pump.mm
/// @brief Draining AppKit events into the guest message queue.
///
/// Two functions, one seam. Win32 guests drive their own message loop, calling
/// GetMessage or PeekMessage and expecting to block or poll on *their* queue.
/// AppKit expects the application to run NSApplication's loop and receive
/// events by callback. Neither will yield to the other.
///
/// `pump_events` reconciles them by pulling whatever AppKit has ready,
/// translating it, and posting it to the guest queue without blocking.
/// `wait_events` blocks when the guest asked to wait, while staying responsive
/// enough that the window server does not consider the app hung.
///
/// This must run on the main thread — AppKit requires it — which is why the
/// guest's message loop is pinned there and worker threads reach it through
/// the queue rather than calling in directly.
///
/// Split out of cocoa_bridge.mm. Both are declared in cocoa_bridge.hpp.

#include "windowing/cocoa/cocoa_perf_overlay.h"
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/messages/input_latency.hpp"
#include "windowing/cocoa/app_icon_presentation.hpp"
#include "windowing/model/diag_flags.hpp"
#include "graphics/common/perf_diag.hpp"
#include "graphics/common/surface_presenter.hpp"
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <algorithm>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreGraphics/CGEventSource.h>
#include <mach/mach_time.h>

#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_bridge.hpp"

namespace vitreo::windowing {

namespace {
// Only the main thread changes depth; signal producers read it to decide
// whether to post a wake event. Nested pumps must leave that event for the
// enclosing nextEventMatchingMask read.
std::atomic<unsigned> native_event_wait_depth{0};
struct NativeEventWaitScope {
    NativeEventWaitScope() { ++native_event_wait_depth; }
    ~NativeEventWaitScope() { --native_event_wait_depth; }
};
// Main-thread only. Translating an event can synchronously query a guest
// wndproc, whose wait services queued native work. A second pump must not
// dequeue the following input before this event has reached its guest queue.
// Learn, About Messages and Message Queues: system input is routed one at a
// time and appended in FIFO order. Keep native-work servicing reentrant;
// serialize only AppKit dequeue/dispatch/translation across both drain paths.
unsigned native_event_dispatch_depth = 0;
struct NativeEventDispatchScope {
    const bool acquired = native_event_dispatch_depth == 0;
    NativeEventDispatchScope() { if (acquired) ++native_event_dispatch_depth; }
    ~NativeEventDispatchScope() { if (acquired) --native_event_dispatch_depth; }
};
} // namespace

bool vtr_main_event_wait_active() noexcept {
    return native_event_wait_depth.load(std::memory_order_seq_cst) != 0;
}

void CocoaBridge::post_client_mouse_message(MessageQueue& queue, HWND target,
                                             u32 mouse_message, u64 w_param,
                                             u64 l_param) {
    // MA_ACTIVATEANDEAT / MA_NOACTIVATEANDEAT: the click's button-down is
    // discarded (Learn WM_MOUSEACTIVATE); its button-up is still delivered.
    if (mouse_message == WM::LBUTTONDOWN || mouse_message == WM::LBUTTONDBLCLK ||
        mouse_message == WM::RBUTTONDOWN || mouse_message == WM::RBUTTONDBLCLK ||
        mouse_message == WM::MBUTTONDOWN) {
        HWND eat = target;
        if (m_mouse_activate_eat_hwnd.compare_exchange_strong(eat, NULL_HWND,
                                                              std::memory_order_acq_rel)) {
            return;
        }
    }
    i32 client_x = static_cast<i16>(l_param & 0xFFFFu);
    i32 client_y = static_cast<i16>((l_param >> 16) & 0xFFFFu);
    // Capture the event's screen point before translating its client point
    // into a child HWND. GetMessagePos must not sample a later cursor move.
    Point screen_point{client_x, client_y};
    const auto source = m_windows.find(target);
    if (source != m_windows.end() && source->second.ns_window) {
        NSWindow* window = reinterpret_cast<NSWindow*>(source->second.ns_window);
        NSView* content = [window contentView];
        const NSPoint content_point = NSMakePoint(client_x, client_y);
        const NSPoint window_point = [content convertPoint:content_point toView:nil];
        const NSPoint fallback = vtr_win32_point_in_view(content, content_point);
        screen_point = {static_cast<i32>(fallback.x), static_cast<i32>(fallback.y)};
        (void)map_native_point_to_guest_surface(window, window_point,
            source->second.fullscreen, screen_point.x, screen_point.y);
        if (!source->second.fullscreen) {
            screen_point.x += source->second.frame.left;
            screen_point.y += source->second.frame.top;
        }
    }
    // Mouse capture. Learn (SetCapture): "the window receives all mouse input
    // until the application calls ReleaseCapture", regardless of which window
    // the pointer is actually over, and lParam is in the CAPTURE window's
    // client coordinates. Routing by pointer position instead left a
    // a guest waiting on a captured window for events that
    // kept going somewhere else: it shows a tooltip, takes capture, and from
    // then on the app painted normally but no longer scrolled or responded
    // to clicks.
    const HWND capture = m_mouse_capture_hwnd.load(std::memory_order_acquire);
    HWND containing_hwnd = target;
    if (capture != NULL_HWND) {
        const auto capture_it = m_windows.find(capture);
        if (capture_it != m_windows.end()) {
            client_x = screen_point.x - capture_it->second.frame.left;
            client_y = screen_point.y - capture_it->second.frame.top;
        } else if (source != m_windows.end() && source->second.ns_window) {
            // A child HWND holds the capture (a list box captures on
            // WM_LBUTTONDOWN): lParam is in ITS client coordinates, so map
            // the content point through its client view — the guest-painted
            // surface when it has one, else its control view.
            NSView* child_view = nil;
            const auto pixel_it = m_guest_pixel_views.find(capture);
            if (pixel_it != m_guest_pixel_views.end() && pixel_it->second != nullptr) {
                child_view = (__bridge NSView*)pixel_it->second;
            } else if (const auto ctrl_it = m_controls.find(capture);
                       ctrl_it != m_controls.end() && ctrl_it->second != nullptr) {
                child_view = (__bridge NSView*)ctrl_it->second;
            }
            NSWindow* window = reinterpret_cast<NSWindow*>(source->second.ns_window);
            if (child_view != nil && [child_view window] == window) {
                NSView* content = [window contentView];
                const NSPoint child_point = vtr_win32_point_in_view(
                    child_view, [child_view convertPoint:NSMakePoint(client_x, client_y)
                                                fromView:content]);
                client_x = static_cast<i32>(std::floor(child_point.x));
                client_y = static_cast<i32>(std::floor(child_point.y));
            }
        }
        target = capture;
        containing_hwnd = capture;
    } else {
        target = retarget_presentation_mouse(target, client_x, client_y,
                                             &containing_hwnd);
    }
    l_param = (static_cast<u64>(static_cast<u16>(client_y)) << 16) |
              static_cast<u16>(client_x);

    // TrackMouseEvent is armed on the HWND that RECEIVES the mouse messages
    // (the routed child or capture window), so leave/hover are judged against
    // that same HWND, in its client coordinates. Judging them against the
    // pre-routing top-level window posted WM_MOUSELEAVE to a tracked child on
    // every move (Learn, WM_MOUSELEAVE: posted when the cursor leaves the
    // client area of the tracked window), and each leave/enter pair re-ran
    // the guest's hover handling.
    m_last_mouse_x = client_x;
    m_last_mouse_y = client_y;
    flush_mouse_tracking(queue, target, client_x, client_y);

    // WM_SETCURSOR: wParam is the HWND containing the cursor. LOWORD(lParam)
    // is the hit-test result and HIWORD(lParam) is the triggering mouse
    // message. These AppKit events are already known to be in the content
    // view, so the hit-test result is HTCLIENT (1).
    queue.remove_by_type(target, WM::SETCURSOR);
    MSG cursor{};
    cursor.hwnd = target;
    cursor.message = WM::SETCURSOR;
    // WM_SETCURSOR names the HWND that physically contains the cursor in
    // wParam. A hosted peer child has no wndproc in this process, so the
    // message is dispatched to its nearest local parent, but the parent must
    // still observe the peer child's HWND just as it does when DefWindowProc
    // gives a Windows parent first chance to set a child's cursor.
    cursor.wParam = containing_hwnd;
    cursor.lParam = (static_cast<u64>(mouse_message & 0xFFFFu) << 16) | 1u;
    cursor.time = static_cast<u32>(continuous_tick_count_ms());
    cursor.pt = screen_point;
    queue.post(cursor);

    // VITREO_MOUSE_DIAG: one short line per mouse message. Deliberately far
    // cheaper than VITREO_TRACE, which slows the guest enough to hide the
    // tooltip input stall entirely.
    // VITREO_MOUSE_DIAG=<path>: one short line per mouse message into a
    // BUFFERED file. VITREO_TRACE writes unbuffered stderr for every API call,
    // which slows the guest enough to hide this stall completely, so the probe
    // that looks for it must not do that.
    static std::FILE* const mouse_diag = [] () -> std::FILE* {
        const char* path = std::getenv("VITREO_MOUSE_DIAG");
        if (path == nullptr || *path == '\0') return nullptr;
        std::FILE* f = std::fopen(path, "w");
        if (f != nullptr) std::atexit([] { });
        return f;
    }();
    if (mouse_diag != nullptr) {
        std::fprintf(mouse_diag, "msg=%x hwnd=%x cap=%x pt=%d,%d\n",
                     mouse_message, target,
                     m_mouse_capture_hwnd.load(std::memory_order_relaxed),
                     client_x, client_y);
    }

    MSG mouse{};
    mouse.hwnd = target;
    mouse.message = mouse_message;
    mouse.wParam = w_param;
    mouse.lParam = l_param;
    mouse.time = cursor.time;
    mouse.pt = screen_point;
    queue.post_input(mouse);
}

HWND CocoaBridge::retarget_presentation_mouse(HWND target, i32& client_x,
                                               i32& client_y,
                                               HWND* containing_hwnd) const {
    if (containing_hwnd != nullptr) {
        *containing_hwnd = target;
    }
    const auto window_it = m_windows.find(target);
    if (window_it == m_windows.end() || window_it->second.ns_window == nullptr) {
        return target;
    }

    NSWindow* window =
        reinterpret_cast<NSWindow*>(window_it->second.ns_window);
    // Host events are view points. Map through the presented surface here
    // in every mode — including exclusive, where a 1920×1200 backbuffer is
    // aspect-fitted into an 1800×1169 point view.
    const bool apply_drawable_map = retarget_applies_presentation_drawable_map(
        window_it->second.fullscreen);
    const bool clamp_letterbox = window_it->second.fullscreen;
    NSView* content = [window contentView];
    const NSPoint content_point = NSMakePoint(client_x, client_y);
    // AppKit hitTest takes the receiver's SUPERview coordinates.
    NSView* view = [content hitTest:[content convertPoint:content_point toView:content.superview]];
    if (diag::input_diag()) {
        static unsigned presentation_hit_logs = 0;
        if (presentation_hit_logs++ < 8) {
            std::fprintf(stderr,
                         "[input] presentation hit target=0x%x point=(%d,%d) "
                         "view=%s presentations=%zu\n",
                         target, client_x, client_y,
                         view != nil
                             ? [NSStringFromClass([view class]) UTF8String]
                             : "nil",
                         m_presentation_views.size());
            for (const auto& [child, native_view] : m_presentation_views) {
                if (native_view == nullptr) continue;
                NSView* presentation = (__bridge NSView*)native_view;
                const NSPoint local = [presentation
                    convertPoint:content_point
                     fromView:content];
                const NSRect bounds = [presentation bounds];
                std::fprintf(stderr,
                             "[input] presentation child=0x%x class=%s "
                             "local=(%.1f,%.1f) bounds=(%.1f,%.1f %.1fx%.1f) "
                             "inside=%d hidden=%d same-window=%d\n",
                             child,
                             [NSStringFromClass([presentation class]) UTF8String],
                             local.x, local.y, bounds.origin.x, bounds.origin.y,
                             bounds.size.width, bounds.size.height,
                             NSPointInRect(local, bounds) ? 1 : 0,
                             [presentation isHiddenOrHasHiddenAncestor] ? 1 : 0,
                             [presentation window] == window ? 1 : 0);
            }
        }
    }
    HWND transport_hit = NULL_HWND;
    while (view != nil && view != content) {
        // A hidden transport subview leaves its visible peer container in
        // the hit path. Keep that containing HWND while routing input to
        // the nearest local procedure on an ancestor.
        for (const HWND child : m_hosted_peer_children) {
            const auto peer = m_controls.find(child);
            if (peer != m_controls.end() && peer->second != nullptr &&
                (__bridge NSView*)peer->second == view && transport_hit == NULL_HWND)
                transport_hit = child;
        }
        // SysLink is drawn natively but its input belongs to the HWND's
        // class procedure (LM_HITTEST / NM_CLICK), like a pixel view.
        if ([view isKindOfClass:[VtrPixelView class]] ||
            [view isKindOfClass:[VtrSysLinkView class]]) {
            for (const auto& [child, native_view] : m_controls) {
                if (native_view != nullptr &&
                    (__bridge NSView*)native_view == view) {
                    if (containing_hwnd != nullptr) *containing_hwnd = transport_hit != NULL_HWND ? transport_hit : child;
                    const NSPoint child_point = vtr_win32_point_in_view(
                        view, [view convertPoint:content_point fromView:content]);
                    client_x = static_cast<i32>(child_point.x);
                    client_y = static_cast<i32>(child_point.y);
                    return child;
                }
            }
        }
        for (const auto& [child, native_view] : m_guest_pixel_views) {
            if (native_view != nullptr &&
                (__bridge NSView*)native_view == view) {
                if (containing_hwnd != nullptr) *containing_hwnd = transport_hit != NULL_HWND ? transport_hit : child;
                const NSPoint child_point = vtr_win32_point_in_view(
                    view, [view convertPoint:content_point fromView:content]);
                client_x = static_cast<i32>(child_point.x);
                client_y = static_cast<i32>(child_point.y);
                return child;
            }
        }
        for (const auto& [child, native_view] : m_presentation_views) {
            if (native_view != nullptr &&
                (__bridge NSView*)native_view == view) {
                if (containing_hwnd != nullptr) *containing_hwnd = child;
                // Cross-process peer hosts are NSViews only — there is no local
                // WindowData/wndproc. Retargeting posts WM_* to that HWND and
                // MessageLoopManager drops them ("hwnd not found"), so clicks
                // appear to do nothing. Route to the nearest local ancestor
                // HWND. A peer compositor is commonly nested under a browser
                // child, so merely keeping the original top-level HWND sends
                // WM_SETCURSOR/WM_MOUSEMOVE to the wrong window and can leave
                // the cursor hidden.
                if (m_hosted_peer_children.find(child) !=
                    m_hosted_peer_children.end()) {
                    for (NSView* ancestor = [view superview];
                         ancestor != nil && ancestor != content;
                         ancestor = [ancestor superview]) {
                        for (const auto& [owner, owner_view] : m_controls) {
                            if (owner_view == nullptr ||
                                (__bridge NSView*)owner_view != ancestor ||
                                m_hosted_peer_children.find(owner) !=
                                    m_hosted_peer_children.end()) {
                                continue;
                            }
                            const NSPoint owner_point =
                                vtr_win32_point_in_view(
                                    ancestor,
                                    [ancestor convertPoint:content_point
                                                   fromView:content]);
                            client_x = static_cast<i32>(owner_point.x);
                            client_y = static_cast<i32>(owner_point.y);
                            if (diag::input_diag()) {
                                std::fprintf(
                                    stderr,
                                    "[input] peer present retarget child=0x%x "
                                    "owner=0x%x\n",
                                    child, owner);
                            }
                            return owner;
                        }
                    }
                    if (diag::input_diag()) {
                        std::fprintf(stderr,
                                     "[input] skip peer present retarget "
                                     "child=0x%x keep=0x%x\n",
                                     child, target);
                    }
                    return target;
                }
                const NSPoint child_point =
                    [view convertPoint:content_point fromView:content];
                // The presentation view can be larger than its drawable:
                // the CAMetalLayer is aspect-fitted inside the view. Win32
                // client coordinates address the drawable, so pointer
                // input must pass through the same viewport transform as
                // the rendered pixels rather than using raw view-local
                // coordinates (which include any letterbox inset).
                //
                if (!apply_drawable_map) {
                    return child;
                }
                if ([view isKindOfClass:[VtrMetalView class]] && view.layer) {
                    for (CALayer* sublayer in view.layer.sublayers) {
                        if (![sublayer isKindOfClass:[CAMetalLayer class]]) {
                            continue;
                        }
                        CAMetalLayer* metal_layer =
                            static_cast<CAMetalLayer*>(sublayer);
                        const CGSize drawable = metal_layer.drawableSize;
                        const NSRect bounds = [view bounds];
                        const graphics::PresentationRect fitted =
                            graphics::aspect_fit_rect(
                                drawable.width, drawable.height,
                                bounds.size.width, bounds.size.height);
                        Point mapped{};
                        if (!map_presentation_point(
                                child_point.x, child_point.y,
                                fitted.x, fitted.y,
                                fitted.width, fitted.height,
                                static_cast<u32>(drawable.width),
                                static_cast<u32>(drawable.height), mapped,
                                clamp_letterbox)) {
                            // A letterbox bar is not part of the child
                            // surface. Leave it targeted at the containing
                            // top-level window in its original coordinates.
                            return target;
                        }
                        client_x = mapped.x;
                        client_y = mapped.y;
                        return child;
                    }
                }
                // Tests, early attachment, and non-Metal presentation
                // views have no drawable transform to apply.
                client_x = static_cast<i32>(child_point.x);
                client_y = static_cast<i32>(child_point.y);
                return child;
            }
        }
        view = [view superview];
    }

    // A cross-process child can be composited beneath a local framebuffer
    // plate. AppKit then hit-tests the plate even though Windows still regards
    // the foreign child HWND as the window containing the pointer. Preserve
    // local dispatch, but recover WM_SETCURSOR.wParam from the hosted peer's
    // view geometry. This is also required while the peer's transport-only
    // presentation subview is hidden after an out-of-process swap-chain
    // handoff; the peer container remains the Windows child surface.
    HWND peer_hit = NULL_HWND;
    NSView* peer_hit_view = nil;
    int peer_hit_depth = -1;
    for (const HWND child : m_hosted_peer_children) {
        const auto control = m_controls.find(child);
        if (control == m_controls.end() || control->second == nullptr) {
            continue;
        }
        NSView* candidate = (__bridge NSView*)control->second;
        if ([candidate window] != window ||
            [candidate isHiddenOrHasHiddenAncestor]) {
            continue;
        }
        const NSPoint local = [candidate convertPoint:content_point
                                             fromView:content];
        if (!NSPointInRect(local, [candidate bounds])) {
            continue;
        }

        int depth = 0;
        for (NSView* ancestor = candidate; ancestor != nil;
             ancestor = [ancestor superview]) {
            ++depth;
        }
        bool prefer = peer_hit_view == nil || depth > peer_hit_depth;
        if (!prefer && depth == peer_hit_depth &&
            [candidate superview] == [peer_hit_view superview]) {
            NSArray<NSView*>* siblings = [[candidate superview] subviews];
            const NSUInteger candidate_index =
                [siblings indexOfObjectIdenticalTo:candidate];
            const NSUInteger current_index =
                [siblings indexOfObjectIdenticalTo:peer_hit_view];
            prefer = candidate_index != NSNotFound &&
                     current_index != NSNotFound &&
                     candidate_index > current_index;
        }
        if (prefer) {
            peer_hit = child;
            peer_hit_view = candidate;
            peer_hit_depth = depth;
        }
    }

    if (peer_hit_view != nil) {
        if (containing_hwnd != nullptr) {
            *containing_hwnd = peer_hit;
        }
        for (NSView* ancestor = [peer_hit_view superview];
             ancestor != nil && ancestor != content;
             ancestor = [ancestor superview]) {
            for (const auto& [owner, owner_view] : m_controls) {
                if (owner_view == nullptr ||
                    (__bridge NSView*)owner_view != ancestor ||
                    m_hosted_peer_children.find(owner) !=
                        m_hosted_peer_children.end()) {
                    continue;
                }
                const NSPoint owner_point = vtr_win32_point_in_view(
                    ancestor,
                    [ancestor convertPoint:content_point fromView:content]);
                client_x = static_cast<i32>(owner_point.x);
                client_y = static_cast<i32>(owner_point.y);
                if (diag::input_diag()) {
                    std::fprintf(stderr,
                                 "[input] peer geometry retarget child=0x%x "
                                 "owner=0x%x\n",
                                 peer_hit, owner);
                }
                return owner;
            }
        }
    }
    return target;
}



// =============================================================================
// Event Pump
// =============================================================================

namespace {
std::atomic<bool> g_main_runloop_drain_active{false};
} // namespace

bool CocoaBridge::main_runloop_drain_active() noexcept {
    return g_main_runloop_drain_active.load(std::memory_order_acquire);
}

void CocoaBridge::install_main_runloop_drain() {
    // VITREO_MAIN_DRAIN_OFF is the off switch for the main-run-loop drain: with
    // it set, MessageLoopManager keeps its poll-pump path (an A/B arm for input
    // delivery latency). pump_events calls this on every pump, so read it once.
    static const bool drain_off = (std::getenv("VITREO_MAIN_DRAIN_OFF") != nullptr);
    if (drain_off) return;
    if (![NSThread isMainThread]) return;
    static CFRunLoopObserverRef observer = nullptr;
    if (observer != nullptr) return;

    // BeforeWaiting: AppKit has delivered whatever arrived and the loop is
    // about to sleep. Draining here turns input delivery from "whenever the
    // guest next polls" into "as soon as macOS has it".
    observer = CFRunLoopObserverCreateWithHandler(
        kCFAllocatorDefault, kCFRunLoopBeforeWaiting, /*repeats=*/true,
        /*order=*/0,
        ^(CFRunLoopObserverRef, CFRunLoopActivity) {
            MessageQueue* queue = m_queue;
            if (queue == nullptr) return;
            // pump_events runs its body inline when already on the main
            // thread, and re-entering it from inside its own drain would
            // recurse through sendEvent. One flag, main thread only.
            if (native_event_dispatch_depth != 0 || native_event_wait_depth != 0) return;
            pump_events(*queue);
        });
    if (observer != nullptr) {
        CFRunLoopAddObserver(CFRunLoopGetMain(), observer,
                             kCFRunLoopCommonModes);
        g_main_runloop_drain_active.store(true, std::memory_order_release);
    }
}

void CocoaBridge::pump_events(MessageQueue& queue) {
    if (diag::win_diag()) {
        static int s_dbg = 0;
        if (++s_dbg == 20) {
            for (auto it = m_controls.begin(); it != m_controls.end(); ++it) {
                NSView* v = (__bridge NSView*)it->second;
                if (![v isKindOfClass:[NSButton class]]) continue;
                NSRect fr = [v frame];
                fprintf(stderr, "[post] hwnd=0x%x frame=(%.0f,%.0f,%.0f,%.0f) '%s'\n",
                        (unsigned)it->first, fr.origin.x, fr.origin.y, fr.size.width, fr.size.height,
                        [[(NSButton*)v title] UTF8String]);
            }
        }
    }

    // Fast bail: if NSApplication hasn't been initialized yet (no CreateWindow
    // or other windowing call), there's nothing to pump.  This avoids the
    // expensive dispatch_sync roundtrip for pure console/extraction apps.
    if (!m_app_initialized) {
        return;
    }
    // Teardown: never block a worker on the main run loop — execute_pe joins
    // those workers from the main thread after begin_shutdown().
    if (m_shutdown.load(std::memory_order_acquire)) {
        return;
    }

    // Measured from here rather than function entry: the two bails above are
    // the uninteresting cases, and counting them would dilute the histogram
    // that has to answer "does pumping stall the guest loop".
    perf::count(perf::Counter::pump_calls);
    perf::Span pump_span(perf::Hist::pump_duration);

    // During live resize the main thread is inside AppKit's tracking loop.
    // Any sync dispatch would stall the guest thread waiting for the tracking
    // loop to finish.  Skip entirely — windowDidResize posts WM_SIZE/WM_PAINT
    // directly to the queue, and the NSTimer blits pending pixels to the layer.
    if (m_in_live_resize.load(std::memory_order_relaxed)) {
        return;
    }
    // The block that drains Cocoa events and translates them into WM_ messages.
    dispatch_block_t blk = ^{
        // A queued worker pump can run inside nextEventMatchingMask's nested
        // run loop too. Leave its wake event for the enclosing native wait.
        if (native_event_wait_depth != 0) return;
        NativeEventDispatchScope dispatching;
        if (!dispatching.acquired) return;
        ensure_app_initialized();

        // Only re-patch delegate/handler queue pointers when the queue
        // identity actually changes. A launcher spins on PeekMessage,
        // and iterating every window/button/menu handler on each pump is
        // pure overhead when the queue pointer is unchanged.
        const bool queue_changed = (m_queue != &queue);
        m_queue = &queue;
        // m_queue is now known, so the run-loop observer has something to
        // drain into. Idempotent.
        install_main_runloop_drain();

        if (queue_changed) {
            // Update all window delegates and button handlers with the current
            // queue pointer so that delegate callbacks (windowDidResize, etc.)
            // can post messages directly into the guest message queue.
            for (auto& [hwnd, state] : m_windows) {
                if (state.ns_delegate) {
                    VtrWindowDelegate* del = reinterpret_cast<VtrWindowDelegate*>(state.ns_delegate);
                    del.queue = &queue;
                }
                for (void* handler_ptr : state.button_handlers) {
                    VtrButtonHandler* handler = (__bridge VtrButtonHandler*)handler_ptr;
                    handler.queue = &queue;
                }
            }

            // Update menu item handlers with the current queue pointer.
            // Menu handlers are installed before the message loop starts (when
            // m_queue is still null), so they need patching on first connect.
            if (NSMutableArray* handlers = vtr_menu_handlers()) {
                for (VtrMenuItemHandler* handler in handlers) {
                    handler.queue = &queue;
                }
            }
        }

        // Drain all pending NSEvents and translate to WM_ messages (non-blocking).
        for (;;) {
            @autoreleasepool {
                NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:[NSDate distantPast]
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES];
                if (!event) break;
                if (vtr_consume_main_wake_event((__bridge void*)event)) continue;

                // How stale is this event? AppKit timestamps it on arrival and
                // CACurrentMediaTime shares that base, so this is exactly the
                // latency added before the guest can even be told. It is the
                // half of input latency the queue probe structurally cannot
                // see, and the half a pump interval lands in.
                switch ([event type]) {
                    case NSEventTypeLeftMouseDown:
                    case NSEventTypeLeftMouseUp:
                    case NSEventTypeRightMouseDown:
                    case NSEventTypeRightMouseUp:
                    case NSEventTypeMouseMoved:
                    case NSEventTypeLeftMouseDragged:
                    case NSEventTypeScrollWheel:
                    case NSEventTypeKeyDown:
                    case NSEventTypeKeyUp: {
                        const double age =
                            CACurrentMediaTime() - [event timestamp];
                        if (age > 0.0 && age < 5.0) {
                            input_latency::note_event_age(
                                static_cast<u64>(age * 1e9));
                        }
                        // A move is not an action the UI owes a frame to; a
                        // click, wheel or key is. Only those start the
                        // click-to-pixels clock.
                        switch ([event type]) {
                            case NSEventTypeMouseMoved:
                            case NSEventTypeLeftMouseDragged:
                                break;
                            default:
                                input_latency::note_action_event();
                                break;
                        }
                        break;
                    }
                    default: break;
                }

                // The performance overlay's ⌥⌘P belongs to neither AppKit nor
                // the guest.
                if (vtr_perf_overlay_consume_key_event((__bridge void*)event)) continue;

                // Let AppKit handle the event first (for window chrome, menus, etc.)
                [NSApp sendEvent:event];
                reassert_cursor_for_mouse_event((__bridge void*)event);

                // Translate select events into WM_ messages for guest code
                switch ([event type]) {
                    case NSEventTypeLeftMouseDown: {
                        NSWindow* win = [event window];
                        HWND target = find_hwnd_for_window(win);
                        if (native_chrome_mouse_event((__bridge void*)event, target)) break;
                        // Every mouse-down, before any early-out. The drag
                        // path has two silent drops (unknown window, and a
                        // failed point mapping) and a click that dies in
                        // either is indistinguishable from a click that never
                        // happened.
                        if (diag::input_diag()) {
                            std::fprintf(stderr,
                                "[input] mousedown(A) win=%p hwnd=0x%x\n",
                                (void*)win, static_cast<unsigned>(target));
                        }
                        if (target != NULL_HWND) {
                            NSPoint loc = [event locationInWindow];
                            // Convert to content view coordinates (VtrContentView is flipped,
                            // so viewLoc.y is already top-down like Win32)
                            i32 winX = 0;
                            i32 winY = 0;
                            {
                                bool clamp = false;
                                auto wit = m_windows.find(target);
                                if (wit != m_windows.end()) clamp = wit->second.fullscreen;
                                if (!map_native_mouse_point(win, loc, winX, winY,
                                                        clamp)) {
                                if (diag::input_diag()) {
                                    std::fprintf(stderr,
                                        "[input] mousedown map FAILED "
                                        "hwnd=0x%x\n",
                                        static_cast<unsigned>(target));
                                }
                                break;
                            }
                            }
                            note_native_mouse_motion(winX, winY, (__bridge void*)event, target);
                                if (perform_guest_frame_mouse_down(
                                    (__bridge void*)event, target, winX,
                                    winY)) {
                                break;
                            }
                            MSG msg{};
                            msg.hwnd    = target;
                            // AppKit already applies the system double-click
                            // interval and slop, so clickCount==2 is exactly
                            // Win32's double-click condition. A window class
                            // that registers CS_DBLCLKS uses this to select
                            // every same-type unit on screen.
                            bool dbl = false;
                            if ([event clickCount] == 2) {
                                auto wit = m_windows.find(target);
                                dbl = (wit != m_windows.end() && wit->second.wants_dblclk);
                            }
                            msg.message = dbl ? WM::LBUTTONDBLCLK : WM::LBUTTONDOWN;
                            msg.wParam  = MK_LBUTTON | mouse_modifier_flags(event);
                            msg.lParam  = ((static_cast<u32>(winY) & 0xFFFF) << 16)
                                        | (static_cast<u32>(winX) & 0xFFFF);
                            m_mouse_buttons |= 1; // track left-down
                            note_native_mouse_button_transition(0x01, true, target);
                            post_client_mouse_message(queue, msg.hwnd, msg.message,
                                                      msg.wParam, msg.lParam);
                        }
                        break;
                    }
                    case NSEventTypeLeftMouseUp: {
                        NSWindow* win = [event window];
                        HWND target = find_hwnd_for_window(win);
                        if (native_chrome_mouse_event((__bridge void*)event, target)) break;
                        if (diag::input_diag()) {
                            std::fprintf(stderr,
                                "[input] mouseup(A) win=%p hwnd=0x%x cap=0x%x\n",
                                (void*)win, static_cast<unsigned>(target),
                                m_mouse_capture_hwnd.load(std::memory_order_acquire));
                        }
                        if (target != NULL_HWND) {
                            NSPoint loc = [event locationInWindow];
                            m_mouse_buttons &= ~1u;
                            i32 winX = 0;
                            i32 winY = 0;
                            {
                                bool clamp = false;
                                auto wit = m_windows.find(target);
                                if (wit != m_windows.end()) clamp = wit->second.fullscreen;
                                if (!map_native_mouse_point(win, loc, winX, winY,
                                                        clamp)) {
                                if (diag::input_diag()) {
                                    std::fprintf(stderr,
                                        "[input] mousedown map FAILED "
                                        "hwnd=0x%x\n",
                                        static_cast<unsigned>(target));
                                }
                                break;
                            }
                            }
                            note_native_mouse_motion(winX, winY, (__bridge void*)event, target);
                            note_native_mouse_button_transition(0x01, false, target);
                            MSG msg{};
                            msg.hwnd    = target;
                            msg.message = WM::LBUTTONUP;
                            msg.wParam  = static_cast<u64>(m_mouse_buttons & 0x3)
                                        | mouse_modifier_flags(event);
                            msg.lParam  = ((static_cast<u32>(winY) & 0xFFFF) << 16)
                                        | (static_cast<u32>(winX) & 0xFFFF);
                            post_client_mouse_message(queue, msg.hwnd, msg.message,
                                                      msg.wParam, msg.lParam);
                        }
                        break;
                    }
                    case NSEventTypeRightMouseDown: {
                        NSWindow* win = [event window];
                        HWND target = find_hwnd_for_window(win);
                        if (native_chrome_mouse_event((__bridge void*)event, target)) break;
                        if (target != NULL_HWND) {
                            NSPoint loc = [event locationInWindow];
                            i32 winX = 0;
                            i32 winY = 0;
                            {
                                bool clamp = false;
                                auto wit = m_windows.find(target);
                                if (wit != m_windows.end()) clamp = wit->second.fullscreen;
                                if (!map_native_mouse_point(win, loc, winX, winY,
                                                        clamp)) {
                                if (diag::input_diag()) {
                                    std::fprintf(stderr,
                                        "[input] mousedown map FAILED "
                                        "hwnd=0x%x\n",
                                        static_cast<unsigned>(target));
                                }
                                break;
                            }
                            }
                            note_native_mouse_motion(winX, winY, (__bridge void*)event, target);
                            MSG msg{};
                            msg.hwnd    = target;
                            msg.message = WM::RBUTTONDOWN;
                            msg.wParam  = MK_RBUTTON | mouse_modifier_flags(event);
                            msg.lParam  = ((static_cast<u32>(winY) & 0xFFFF) << 16)
                                        | (static_cast<u32>(winX) & 0xFFFF);
                            m_mouse_buttons |= 2; // track right-down
                            note_native_mouse_button_transition(0x02, true, target);
                            post_client_mouse_message(queue, msg.hwnd, msg.message,
                                                      msg.wParam, msg.lParam);
                        }
                        break;
                    }
                    case NSEventTypeRightMouseUp: {
                        NSWindow* win = [event window];
                        HWND target = find_hwnd_for_window(win);
                        if (native_chrome_mouse_event((__bridge void*)event, target)) break;
                        if (target != NULL_HWND) {
                            NSPoint loc = [event locationInWindow];
                            m_mouse_buttons &= ~2u;
                            i32 winX = 0;
                            i32 winY = 0;
                            {
                                bool clamp = false;
                                auto wit = m_windows.find(target);
                                if (wit != m_windows.end()) clamp = wit->second.fullscreen;
                                if (!map_native_mouse_point(win, loc, winX, winY,
                                                        clamp)) {
                                if (diag::input_diag()) {
                                    std::fprintf(stderr,
                                        "[input] mousedown map FAILED "
                                        "hwnd=0x%x\n",
                                        static_cast<unsigned>(target));
                                }
                                break;
                            }
                            }
                            note_native_mouse_motion(winX, winY, (__bridge void*)event, target);
                            note_native_mouse_button_transition(0x02, false, target);
                            MSG msg{};
                            msg.hwnd    = target;
                            msg.message = WM::RBUTTONUP;
                            msg.wParam  = static_cast<u64>(m_mouse_buttons & 0x3)
                                        | mouse_modifier_flags(event);
                            msg.lParam  = ((static_cast<u32>(winY) & 0xFFFF) << 16)
                                        | (static_cast<u32>(winX) & 0xFFFF);
                            post_client_mouse_message(queue, msg.hwnd, msg.message,
                                                      msg.wParam, msg.lParam);
                        }
                        break;
                    }
                    case NSEventTypeLeftMouseDragged:
                    case NSEventTypeRightMouseDragged:
                    case NSEventTypeMouseMoved: {
                        NSWindow* win = [event window];
                        HWND target = find_hwnd_for_window(win);
                        if (native_chrome_mouse_event((__bridge void*)event, target)) break;
                        if (target == NULL_HWND) {
                            note_offwindow_mouse_motion((__bridge void*)event);
                            break;
                        }
                        {
                            NSPoint loc = [event locationInWindow];
                            i32 winX = 0;
                            i32 winY = 0;
                            {
                                bool clamp = false;
                                auto wit = m_windows.find(target);
                                if (wit != m_windows.end()) clamp = wit->second.fullscreen;
                                if (!map_native_mouse_point(win, loc, winX, winY,
                                                        clamp)) {
                                if (diag::input_diag()) {
                                    std::fprintf(stderr,
                                        "[input] mousedown map FAILED "
                                        "hwnd=0x%x\n",
                                        static_cast<unsigned>(target));
                                }
                                break;
                            }
                            }
                            note_native_mouse_motion(winX, winY, (__bridge void*)event, target);
                            // Use internally tracked button state — macOS
                            // reports MouseMoved even while buttons are held.
                            u64 mk = static_cast<u64>(m_mouse_buttons & 0x3) | mouse_modifier_flags(event);
                            // Coalesce: remove any prior WM_MOUSEMOVE for
                            // this hwnd so only the latest position is queued.
                            // Real Windows never queues duplicate mouse-moves.
                            queue.remove_by_type(target, WM::MOUSEMOVE);
                            MSG msg{};
                            msg.hwnd    = target;
                            msg.message = WM::MOUSEMOVE;
                            msg.wParam  = mk;
                            msg.lParam  = ((static_cast<u32>(winY) & 0xFFFF) << 16)
                                        | (static_cast<u32>(winX) & 0xFFFF);
                            post_client_mouse_message(queue, msg.hwnd, msg.message,
                                                      msg.wParam, msg.lParam);
                        }
                        break;
                    }
                    case NSEventTypeScrollWheel: {
                        translate_scroll_wheel((__bridge void*)event, queue);
                        break;
                    }
                    case NSEventTypeKeyDown: {
                        NSWindow* win = [event window];
                        HWND target = keyboard_target(find_hwnd_for_window(win));
                        static const bool latency = std::getenv("VITREO_INPUT_LATENCY") != nullptr;
                        if (latency) {
                            std::fprintf(stderr, "[input-native] hwnd=0x%x code=%u age_ms=%.1f\n",
                                         target, (unsigned)[event keyCode],
                                         ([NSProcessInfo processInfo].systemUptime - event.timestamp) * 1000.0);
                        }
                        // Let AppKit own keystrokes while a native EDIT/RichEdit
                        // editor is focused (see vtr_native_text_editor_focused).
                        if (target != NULL_HWND &&
                            !vtr_native_text_editor_focused(win)) {
                            MSG msg{};
                            msg.hwnd    = target;
                            msg.message = WM::KEYDOWN;
                            msg.wParam  = map_native_keycode_to_windows_vk([event keyCode]);
                            if (diag::input_diag() ||
                                diag::focus_diag()) {
                                std::fprintf(stderr,
                                    "[input] keyDown hwnd=0x%x native=%u vk=0x%02llx "
                                    "focus=0x%x mods=0x%llx\n",
                                    msg.hwnd,
                                    (unsigned)[event keyCode],
                                    (unsigned long long)msg.wParam,
                                    keyboard_focus_hwnd(),
                                    (unsigned long long)[event modifierFlags]);
                            }
                            if (msg.wParam != 0) {
                                const bool was_down = native_key_is_down(
                                    static_cast<u32>(msg.wParam));
                                msg.lParam = key_message_lparam(
                                    static_cast<u32>(msg.wParam), [event modifierFlags],
                                    true, was_down);
                                note_native_key_transition(static_cast<u32>(msg.wParam), true);
                                msg.time = static_cast<u32>(continuous_tick_count_ms());
                                queue.post_input(msg);
                            }
                        }
                        break;
                    }
                    case NSEventTypeKeyUp: {
                        NSWindow* win = [event window];
                        HWND target = keyboard_target(find_hwnd_for_window(win));
                        if (target != NULL_HWND &&
                            !vtr_native_text_editor_focused(win)) {
                            MSG msg{};
                            msg.hwnd    = target;
                            msg.message = WM::KEYUP;
                            msg.wParam  = map_native_keycode_to_windows_vk([event keyCode]);
                            if (msg.wParam != 0) {
                                msg.lParam = key_message_lparam(
                                    static_cast<u32>(msg.wParam), [event modifierFlags],
                                    false, true);
                                note_native_key_transition(static_cast<u32>(msg.wParam), false);
                                msg.time = static_cast<u32>(continuous_tick_count_ms());
                                queue.post_input(msg);
                            }
                        }
                        break;
                    }
                    case NSEventTypeFlagsChanged: {
                        // macOS reports modifier press/release as flagsChanged
                        // rather than keyDown/keyUp, so without this Windows
                        // never sees VK_SHIFT/VK_CONTROL/VK_MENU go down or up.
                        // These events frequently carry no window, so fall back
                        // to the key window the way keyboard focus works.
                        NSWindow* win = [event window];
                        if (!win) win = [NSApp keyWindow];
                        HWND target = keyboard_target(find_hwnd_for_window(win));
                        if (target != NULL_HWND) {
                            post_modifier_transitions(target, [event modifierFlags], queue);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        const HWND pending_activation =
            m_pending_activation_hwnd.load(std::memory_order_acquire);
        if (pending_activation != NULL_HWND) {
            const HWND preferred =
                m_preferred_key_hwnd.load(std::memory_order_acquire);
            // Do not flush a pending makeKey for a non-preferred top-level —
            // that reintroduces sibling key thrash after resign gaps.
            if (preferred != NULL_HWND && preferred != pending_activation) {
                HWND expected_activation = pending_activation;
                m_pending_activation_hwnd.compare_exchange_strong(
                    expected_activation, NULL_HWND,
                    std::memory_order_acq_rel);
            } else if (![NSApp isActive]) {
                // Do not activateIgnoringOtherApps every pump tick while
                // another host app is frontmost — that posts WM_KILLFOCUS /
                // WM_SETFOCUS storms and steals the user's focus.
                // Keep pending so a later genuine activation can finish.
                //
                // Except for the start grant: a process launched by the
                // foreground one (Learn SetForegroundWindow) asks again,
                // because on macOS 14+ the launcher's cooperative yield can
                // arrive after the first request. Activation there is a
                // request the system grants only to a yielded-to app, so
                // asking cannot steal focus; at most every 250 ms.
                if (m_holds_start_grant.load(std::memory_order_acquire)) {
                    if (@available(macOS 14.0, *)) {
                        static CFAbsoluteTime last_request = 0;
                        const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
                        if (now - last_request >= 0.25) {
                            last_request = now;
                            [NSApp activate];
                        }
                    }
                }
            } else {
                // Consume before entering AppKit: ordering may re-enter the
                // run loop, and a refused activation must not be retried on
                // every pump tick. A later explicit activation can enqueue
                // a new request; native callbacks remain the source of truth
                // for whether this window actually became key.
                HWND expected_activation = pending_activation;
                const bool consumed =
                    m_pending_activation_hwnd.compare_exchange_strong(
                        expected_activation, NULL_HWND,
                        std::memory_order_acq_rel);
                auto pending_it = m_windows.find(pending_activation);
                if (consumed && pending_it != m_windows.end() &&
                    pending_it->second.ns_window) {
                    NSWindow* pending_window = reinterpret_cast<NSWindow*>(
                        pending_it->second.ns_window);
                    if (![pending_window isKeyWindow] &&
                        [pending_window canBecomeKeyWindow]) {
                        [pending_window makeKeyAndOrderFront:nil];
                    }
                    if ([pending_window isKeyWindow]) {
                        on_native_window_became_key(pending_activation);
                    }
                }
            }
        }

        // Keep exclusive / ClipCursor confinement honest even if a mouse-moved
        // monitor misses an event (fast multi-monitor flicks).
        enforce_cursor_clip();

        // In a normal AppKit app this happens each runloop iteration.
        // Vitreo drives AppKit manually, so force a display/update tick.
        [NSApp updateWindows];
    };
    // Use dispatch_sync for minimum overhead — this fires every ~1000
    // instructions and must be fast.  We already early-return when
    // m_in_live_resize is true, so we will never hit the case where
    // [NSApp sendEvent:] enters a tracking loop inside this block.
    if ([NSThread isMainThread]) {
        blk();
    } else {
        // Measured separately because this is the expensive shape: the main
        // thread only services the dispatch queue when it reaches
        // nextEventMatchingMask, which its GetMessage loop does every ~16 ms.
        // A worker blocked here waits for that, so a burst of worker-thread UI
        // calls costs ~16 ms each no matter how little work the block does.
        perf::count(perf::Counter::pump_cross_thread);
        perf::Span wait_span(perf::Hist::pump_cross_thread_wait);
        // CFRunLoopPerformBlock + wake, with bailout on live-resize *or*
        // process teardown so a worker cannot deadlock against main-thread
        // wait_for_threads after guest ExitProcess/crash.
        vtr_run_on_main_sync_bailout(blk, m_in_live_resize, &m_shutdown);
    }
}

// =============================================================================
// Event Wait
// =============================================================================

void CocoaBridge::wait_events(MessageQueue& queue, u32 timeout_ms) {
    (void)wait_events_impl(queue, timeout_ms, {});
}

void CocoaBridge::wait_events(MessageQueue& queue, u32 timeout_ms,
                              std::function<bool()> ready) {
    (void)wait_events_impl(queue, timeout_ms, std::move(ready));
}

bool CocoaBridge::wait_events_waited(MessageQueue& queue, u32 timeout_ms) {
    return wait_events_impl(queue, timeout_ms, {});
}

bool CocoaBridge::wait_events_impl(MessageQueue& queue, u32 timeout_ms,
                                   std::function<bool()> ready) {
    // The main thread waiting (e.g. for a SendMessage reply) still runs what
    // guest threads queued for it: the thread it waits for may be waiting on
    // exactly that.
    vtr_drain_main_work();
    // Same as pump_events: skip the sync roundtrip during live resize / teardown.
    if (m_in_live_resize.load(std::memory_order_relaxed) ||
        m_shutdown.load(std::memory_order_acquire)) {
        return false;
    }
    // nextEventMatchingMask before finishLaunching parks forever (no runloop
    // drain). Unit tests and headless bring-up that never created a window
    // must fall through to the MessageQueue CV wait instead.
    if (!m_app_launched) {
        return false;
    }
    // Set only once the block owns the dispatch scope, so a bailout from
    // vtr_run_on_main_sync_bailout (live resize or teardown arriving while
    // queued) also reports "did not wait".
    __block bool waited = false;
    vtr_run_on_main_sync_bailout(^{
        NativeEventDispatchScope dispatching;
        if (!dispatching.acquired) return;
        waited = true;
        ensure_app_initialized();

        // Cache queue pointer for delegate callbacks
        m_queue = &queue;

        // Update all window delegates and button handlers with the current
        // queue pointer so that delegate callbacks (windowDidResize, etc.)
        // can post messages directly into the guest message queue.
        for (auto& [hwnd, state] : m_windows) {
            if (state.ns_delegate) {
                VtrWindowDelegate* del = reinterpret_cast<VtrWindowDelegate*>(state.ns_delegate);
                del.queue = &queue;
            }
            for (void* handler_ptr : state.button_handlers) {
                VtrButtonHandler* handler = (__bridge VtrButtonHandler*)handler_ptr;
                handler.queue = &queue;
            }
        }

        // Update menu item handlers with the current queue pointer.
        if (NSMutableArray* handlers = vtr_menu_handlers()) {
            for (VtrMenuItemHandler* handler in handlers) {
                handler.queue = &queue;
            }
        }

        @autoreleasepool {
            NSDate* untilDate = (timeout_ms == 0)
                ? [NSDate distantPast]
                : [NSDate dateWithTimeIntervalSinceNow:(static_cast<double>(timeout_ms) / 1000.0)];

            NSEvent* event = nil;
            {
                NativeEventWaitScope waiting;
                // Publish the active native wait before checking completion:
                // a reply is either observed here or posts an event below.
                if ((ready && ready()) || vtr_main_wake_pending()) return;
                event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:untilDate
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
            }
            // A wake event ends the wait; there is nothing to dispatch.
            if (event != nil && vtr_consume_main_wake_event((__bridge void*)event)) event = nil;
            if (event != nil && vtr_perf_overlay_consume_key_event((__bridge void*)event)) {
                event = nil;
            }
            if (event) {
                [NSApp sendEvent:event];
                reassert_cursor_for_mouse_event((__bridge void*)event);

                switch ([event type]) {
                case NSEventTypeLeftMouseDown: {
                    NSWindow* win = [event window];
                    HWND target = find_hwnd_for_window(win);
                    if (native_chrome_mouse_event((__bridge void*)event, target)) break;
                    if (diag::input_diag()) {
                        std::fprintf(stderr,
                            "[input] mousedown(B) win=%p hwnd=0x%x\n",
                            (void*)win, static_cast<unsigned>(target));
                    }
                    if (target != NULL_HWND) {
                        NSPoint loc = [event locationInWindow];
                        i32 winX = 0;
                        i32 winY = 0;
                        {
                            bool clamp = false;
                            auto wit = m_windows.find(target);
                            if (wit != m_windows.end()) clamp = wit->second.fullscreen;
                            if (!map_native_mouse_point(win, loc, winX, winY,
                                                        clamp)) {
                                if (diag::input_diag()) {
                                    std::fprintf(stderr,
                                        "[input] mousedown map FAILED "
                                        "hwnd=0x%x\n",
                                        static_cast<unsigned>(target));
                                }
                                break;
                            }
                        }
                        note_native_mouse_motion(winX, winY, (__bridge void*)event, target);
                        if (perform_guest_frame_mouse_down(
                                (__bridge void*)event, target, winX, winY)) {
                            break;
                        }
                        MSG msg{};
                        msg.hwnd    = target;
                        // Same CS_DBLCLKS handling as the pump_events path —
                        // both drains must agree or double-clicks vanish
                        // whenever the guest happens to be blocking.
                        bool dbl = false;
                        if ([event clickCount] == 2) {
                            auto wit = m_windows.find(target);
                            dbl = (wit != m_windows.end() && wit->second.wants_dblclk);
                        }
                        msg.message = dbl ? WM::LBUTTONDBLCLK : WM::LBUTTONDOWN;
                        msg.wParam  = MK_LBUTTON | mouse_modifier_flags(event);
                        msg.lParam  = ((static_cast<vitreo::u32>(winY) & 0xFFFF) << 16)
                                  | (static_cast<vitreo::u32>(winX) & 0xFFFF);
                        m_mouse_buttons |= 1; // track left-down
                        note_native_mouse_button_transition(0x01, true, target);
                        post_client_mouse_message(queue, msg.hwnd, msg.message,
                                                  msg.wParam, msg.lParam);
                    }
                    break;
                }
                case NSEventTypeLeftMouseUp: {
                    NSWindow* win = [event window];
                    HWND target = find_hwnd_for_window(win);
                    if (native_chrome_mouse_event((__bridge void*)event, target)) break;
                    if (diag::input_diag()) {
                        std::fprintf(stderr,
                            "[input] mouseup(B) win=%p hwnd=0x%x cap=0x%x\n",
                            (void*)win, static_cast<unsigned>(target),
                            m_mouse_capture_hwnd.load(std::memory_order_acquire));
                    }
                    if (target != NULL_HWND) {
                        NSPoint loc = [event locationInWindow];
                        m_mouse_buttons &= ~1u;
                        i32 winX = 0;
                        i32 winY = 0;
                        {
                            bool clamp = false;
                            auto wit = m_windows.find(target);
                            if (wit != m_windows.end()) clamp = wit->second.fullscreen;
                            if (!map_native_mouse_point(win, loc, winX, winY,
                                                        clamp)) {
                                if (diag::input_diag()) {
                                    std::fprintf(stderr,
                                        "[input] mousedown map FAILED "
                                        "hwnd=0x%x\n",
                                        static_cast<unsigned>(target));
                                }
                                break;
                            }
                        }
                        note_native_mouse_motion(winX, winY, (__bridge void*)event, target);
                        note_native_mouse_button_transition(0x01, false, target);
                        MSG msg{};
                        msg.hwnd    = target;
                        msg.message = WM::LBUTTONUP;
                        msg.wParam  = static_cast<vitreo::u64>(m_mouse_buttons & 0x3)
                                    | mouse_modifier_flags(event);
                        msg.lParam  = ((static_cast<vitreo::u32>(winY) & 0xFFFF) << 16)
                                  | (static_cast<vitreo::u32>(winX) & 0xFFFF);
                        post_client_mouse_message(queue, msg.hwnd, msg.message,
                                                  msg.wParam, msg.lParam);
                    }
                    break;
                }
                case NSEventTypeRightMouseDown: {
                    NSWindow* win = [event window];
                    HWND target = find_hwnd_for_window(win);
                    if (native_chrome_mouse_event((__bridge void*)event, target)) break;
                    if (target != NULL_HWND) {
                        NSPoint loc = [event locationInWindow];
                        i32 winX = 0;
                        i32 winY = 0;
                        {
                            bool clamp = false;
                            auto wit = m_windows.find(target);
                            if (wit != m_windows.end()) clamp = wit->second.fullscreen;
                            if (!map_native_mouse_point(win, loc, winX, winY,
                                                        clamp)) {
                                if (diag::input_diag()) {
                                    std::fprintf(stderr,
                                        "[input] mousedown map FAILED "
                                        "hwnd=0x%x\n",
                                        static_cast<unsigned>(target));
                                }
                                break;
                            }
                        }
                        note_native_mouse_motion(winX, winY, (__bridge void*)event, target);
                        MSG msg{};
                        msg.hwnd    = target;
                        msg.message = WM::RBUTTONDOWN;
                        msg.wParam  = MK_RBUTTON | mouse_modifier_flags(event);
                        msg.lParam  = ((static_cast<vitreo::u32>(winY) & 0xFFFF) << 16)
                                  | (static_cast<vitreo::u32>(winX) & 0xFFFF);
                        m_mouse_buttons |= 2; // track right-down
                        note_native_mouse_button_transition(0x02, true, target);
                        post_client_mouse_message(queue, msg.hwnd, msg.message,
                                                  msg.wParam, msg.lParam);
                    }
                    break;
                }
                case NSEventTypeRightMouseUp: {
                    NSWindow* win = [event window];
                    HWND target = find_hwnd_for_window(win);
                    if (native_chrome_mouse_event((__bridge void*)event, target)) break;
                    if (target != NULL_HWND) {
                        NSPoint loc = [event locationInWindow];
                        m_mouse_buttons &= ~2u;
                        i32 winX = 0;
                        i32 winY = 0;
                        {
                            bool clamp = false;
                            auto wit = m_windows.find(target);
                            if (wit != m_windows.end()) clamp = wit->second.fullscreen;
                            if (!map_native_mouse_point(win, loc, winX, winY,
                                                        clamp)) {
                                if (diag::input_diag()) {
                                    std::fprintf(stderr,
                                        "[input] mousedown map FAILED "
                                        "hwnd=0x%x\n",
                                        static_cast<unsigned>(target));
                                }
                                break;
                            }
                        }
                        note_native_mouse_motion(winX, winY, (__bridge void*)event, target);
                        note_native_mouse_button_transition(0x02, false, target);
                        MSG msg{};
                        msg.hwnd    = target;
                        msg.message = WM::RBUTTONUP;
                        msg.wParam  = static_cast<vitreo::u64>(m_mouse_buttons & 0x3)
                                    | mouse_modifier_flags(event);
                        msg.lParam  = ((static_cast<vitreo::u32>(winY) & 0xFFFF) << 16)
                                  | (static_cast<vitreo::u32>(winX) & 0xFFFF);
                        post_client_mouse_message(queue, msg.hwnd, msg.message,
                                                  msg.wParam, msg.lParam);
                    }
                    break;
                }
                case NSEventTypeLeftMouseDragged:
                case NSEventTypeRightMouseDragged:
                case NSEventTypeMouseMoved: {
                    NSWindow* win = [event window];
                    // A drag delivered while AppKit is resizing the window
                    // belongs to the resize gesture, not to the guest. AppKit
                    // handles the border drag itself and still routes the
                    // events through the normal queue, so without this guard
                    // the guest ALSO sees them as client input.
                    //
                    // Dragging the window corner selected every label in the page,
                    // because the renderer received the gesture as a
                    // click-and-drag text selection while AppKit was resizing.
                    //
                    // Scoped to drags/moves: a plain click is not part of a
                    // sizing gesture, and inLiveResize is false by then anyway.
                    if (win != nil && [win inLiveResize]) {
                        break;
                    }
                    HWND target = find_hwnd_for_window(win);
                    if (native_chrome_mouse_event((__bridge void*)event, target)) break;
                    if (target == NULL_HWND) {
                        note_offwindow_mouse_motion((__bridge void*)event);
                        break;
                    }
                    {
                        NSPoint loc = [event locationInWindow];
                        i32 winX = 0;
                        i32 winY = 0;
                        {
                            bool clamp = false;
                            auto wit = m_windows.find(target);
                            if (wit != m_windows.end()) clamp = wit->second.fullscreen;
                            if (!map_native_mouse_point(win, loc, winX, winY,
                                                        clamp)) {
                                if (diag::input_diag()) {
                                    std::fprintf(stderr,
                                        "[input] mousedown map FAILED "
                                        "hwnd=0x%x\n",
                                        static_cast<unsigned>(target));
                                }
                                break;
                            }
                        }
                        note_native_mouse_motion(winX, winY, (__bridge void*)event, target);
                        // Use internally tracked button state — macOS
                        // reports MouseMoved even while buttons are held.
                        vitreo::u64 mk = static_cast<vitreo::u64>(m_mouse_buttons & 0x3)
                                       | mouse_modifier_flags(event);
                        // Match pump_events and Windows' input queue: when
                        // mouse input arrives faster than the thread consumes
                        // it, retain only the newest move for this window.
                        queue.remove_by_type(target, WM::MOUSEMOVE);
                        MSG msg{};
                        msg.hwnd    = target;
                        msg.message = WM::MOUSEMOVE;
                        msg.wParam  = mk;
                        msg.lParam  = ((static_cast<vitreo::u32>(winY) & 0xFFFF) << 16)
                                  | (static_cast<vitreo::u32>(winX) & 0xFFFF);
                        post_client_mouse_message(queue, msg.hwnd, msg.message,
                                                  msg.wParam, msg.lParam);
                    }
                    break;
                }
                case NSEventTypeScrollWheel: {
                    translate_scroll_wheel((__bridge void*)event, queue);
                    break;
                }
                case NSEventTypeKeyDown: {
                    NSWindow* win = [event window];
                    HWND target = keyboard_target(find_hwnd_for_window(win));
                    if (target != NULL_HWND &&
                        !vtr_native_text_editor_focused(win)) {
                        MSG msg{};
                        msg.hwnd    = target;
                        msg.message = WM::KEYDOWN;
                        msg.wParam  = map_native_keycode_to_windows_vk([event keyCode]);
                        if (diag::input_diag()) {
                            std::fprintf(stderr,
                                "[input] keyDown(alt-pump) native=%u vk=0x%02llx mods=0x%llx\n",
                                (unsigned)[event keyCode],
                                (unsigned long long)msg.wParam,
                                (unsigned long long)[event modifierFlags]);
                        }
                        if (msg.wParam != 0) {
                            const bool was_down = native_key_is_down(
                                static_cast<u32>(msg.wParam));
                            msg.lParam = key_message_lparam(
                                static_cast<u32>(msg.wParam), [event modifierFlags],
                                true, was_down);
                            note_native_key_transition(static_cast<u32>(msg.wParam), true);
                            msg.time = static_cast<u32>(continuous_tick_count_ms());
                            queue.post_input(msg);
                        }
                    }
                    break;
                }
                case NSEventTypeFlagsChanged: {
                    NSWindow* win = [event window];
                    if (!win) win = [NSApp keyWindow];
                    HWND target = keyboard_target(find_hwnd_for_window(win));
                    if (target != NULL_HWND) {
                        post_modifier_transitions(target, [event modifierFlags], queue);
                    }
                    break;
                }
                case NSEventTypeKeyUp: {
                    NSWindow* win = [event window];
                    HWND target = keyboard_target(find_hwnd_for_window(win));
                    if (target != NULL_HWND &&
                        !vtr_native_text_editor_focused(win)) {
                        MSG msg{};
                        msg.hwnd    = target;
                        msg.message = WM::KEYUP;
                        msg.wParam  = map_native_keycode_to_windows_vk([event keyCode]);
                        if (msg.wParam != 0) {
                            msg.lParam = key_message_lparam(
                                static_cast<u32>(msg.wParam), [event modifierFlags],
                                false, true);
                            note_native_key_transition(static_cast<u32>(msg.wParam), false);
                            msg.time = static_cast<u32>(continuous_tick_count_ms());
                            queue.post_input(msg);
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }

            [NSApp updateWindows];
        }
    }, m_in_live_resize, &m_shutdown);

    // Drain remaining events without blocking.
    pump_events(queue);
    return waited;
}

} // namespace vitreo::windowing
