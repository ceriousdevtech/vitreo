// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_hosted_peers.mm
/// @brief Child windows shown here on behalf of a peer process.
///
/// A hosted peer child is the view another guest process presents into on
/// behalf of a window shown here. These functions create it, keep it sized
/// to its window and to what the peer presents, and retire the local host
/// once the peer has taken the child over.
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

bool CocoaBridge::ensure_hosted_peer_child(HWND parent_hwnd, HWND child_hwnd,
                                           const Rect& frame) {
    if (parent_hwnd == NULL_HWND || child_hwnd == NULL_HWND ||
        parent_hwnd == child_hwnd) {
        return false;
    }
    __block bool result = false;
    vtr_run_on_main_sync(^{
        NSView* parent_view = nil;
        const auto parent_control = m_controls.find(parent_hwnd);
        if (parent_control != m_controls.end() &&
            parent_control->second != nullptr) {
            parent_view = (__bridge NSView*)parent_control->second;
        } else {
            const auto parent_window = m_windows.find(parent_hwnd);
            if (parent_window != m_windows.end() &&
                parent_window->second.ns_window != nullptr) {
                parent_view = [reinterpret_cast<NSWindow*>(
                    parent_window->second.ns_window) contentView];
            }
        }
        if (parent_view == nil) return;

        parent_view.clipsToBounds = YES;
        for (NSView* ancestor = parent_view; ancestor != nil;
             ancestor = [ancestor superview]) {
            ancestor.clipsToBounds = YES;
        }

        const CGFloat parent_h = [parent_view bounds].size.height;
        const CGFloat w = static_cast<CGFloat>(
            std::max(1, frame.right - frame.left));
        const CGFloat h = static_cast<CGFloat>(
            std::max(1, frame.bottom - frame.top));
        CGFloat y = static_cast<CGFloat>(frame.top);
        if (parent_view != nil && ![parent_view isFlipped] && parent_h > 0.0) {
            y = parent_h - static_cast<CGFloat>(frame.top) - h;
        }
        const NSRect cocoa_frame = NSMakeRect(
            static_cast<CGFloat>(frame.left), y, w, h);

        NSView* child_view = nil;
        auto existing = m_controls.find(child_hwnd);
        if (existing != m_controls.end() && existing->second != nullptr) {
            child_view = (__bridge NSView*)existing->second;
            if ([child_view superview] != parent_view) {
                [child_view removeFromSuperview];
                // SetParent puts the moved window at the TOP of its new
                // parent's child z-order (tools/oracle_probe/
                // setparent_zorder_probe.expected). This used to be the
                // bottom, to keep an empty Metal plate from burying the legacy
                // compositor child. That was the wrong fix: the hosted view is
                // clear until its first frame, and that child is
                // WS_EX_TRANSPARENT, which Windows composes beneath its
                // siblings anyway (restack_transparent_children). Placed at
                // the bottom, the child's opaque unpainted plate
                // covered every popup's GPU content.
                [parent_view addSubview:child_view
                             positioned:NSWindowAbove
                             relativeTo:nil];
            }
            [child_view setFrame:cocoa_frame];
        } else {
            child_view = [[NSView alloc] initWithFrame:cocoa_frame];
            child_view.clipsToBounds = YES;
            child_view.wantsLayer = YES;
            child_view.layer.backgroundColor = NSColor.clearColor.CGColor;
            [parent_view addSubview:child_view
                         positioned:NSWindowAbove
                         relativeTo:nil];
            m_controls[child_hwnd] = (__bridge void*)child_view;
        }
        restack_transparent_children((__bridge void*)parent_view);
        m_hosted_peer_children.insert(child_hwnd);
        // Tell the owner which process actually shows this child. Its own
        // process presents into it and its parent is local there, so neither
        // the owner nor the parent check can find a transport target; without
        // this the frames land in a detached offscreen view and never reach
        // the screen.
        ipc::hwnd_registry_set_present_host(static_cast<u32>(child_hwnd),
                                            static_cast<u32>(::getpid()));
        m_hosted_peer_parents[child_hwnd] = parent_hwnd;
        m_hosted_peer_origins[child_hwnd] = {frame.left, frame.top};
        if (w > 1.0 && h > 1.0) {
            m_hosted_peer_present_sized.erase(child_hwnd);
        }
        result = true;
        std::fprintf(stderr,
                     "[windowing] hosted peer child HWND=0x%x under "
                     "parent=0x%x frame=(%d,%d %dx%d)\n",
                     child_hwnd, parent_hwnd, frame.left, frame.top,
                     frame.right - frame.left, frame.bottom - frame.top);
    });
    return result;
}

bool CocoaBridge::is_hosted_peer_child(HWND hwnd) const {
    return m_hosted_peer_children.find(hwnd) != m_hosted_peer_children.end();
}

void CocoaBridge::resize_hosted_peer_children_for_window(void* ns_window) {
    // A hosted peer child is the view a peer process presents into: the GPU
    // process, compositing for the browser
    // process's window. It is sized
    // by grow_hosted_peer_child_to_present_size, which is driven off the
    // child's own registered client size -- and nothing republishes that when
    // the USER drags the macOS border, because a native resize never goes
    // through the guest's SetWindowPos.
    //
    // Publishing the TOP-LEVEL window's new size is not enough: the peer
    // presents into a CHILD hwnd with its own registry entry. So resize the
    // child views directly here, from the one place that knows the gesture
    // happened.
    //
    // MEASURED: without this the guest correctly built swapchains at the new
    // size (1784x881) while the peer's view stayed pinned at 1008x722, so the drawable was squeezed into a fixed view --
    // enlarging the window made the UI look SMALLER and shrinking it made the
    // UI look BIGGER, which reads as inverted scaling but is really a
    // fixed-size destination.
    if (ns_window == nullptr) return;
    NSWindow* window = (__bridge NSWindow*)ns_window;
    NSView* content = [window contentView];
    if (content == nil) return;
    const NSRect bounds = [content bounds];

    for (const HWND child : m_hosted_peer_children) {
        auto control = m_controls.find(child);
        if (control == m_controls.end() || control->second == nullptr) continue;
        NSView* child_view = (__bridge NSView*)control->second;
        if ([child_view window] != window) continue;   // different top level
        if (NSEqualRects([child_view frame], bounds)) continue;
        [child_view setFrame:bounds];
        auto presentation = m_presentation_views.find(child);
        if (presentation != m_presentation_views.end() &&
            presentation->second != nullptr) {
            NSView* metal = (__bridge NSView*)presentation->second;
            if ([metal superview] == child_view) {
                [metal setFrame:[child_view bounds]];
            }
        }
    }
}

bool CocoaBridge::grow_hosted_peer_child_to_present_size(HWND hwnd, i32 width,
                                                         i32 height) {
    if (hwnd == NULL_HWND || width < 1 || height < 1) return false;
    // NOT CACHED, deliberately. An earlier version skipped the main-thread
    // round trip when this HWND had already been sized to these dimensions.
    // That is wrong: this function does not only size THIS child, it also
    // walks m_hosted_peer_parents expanding placeholder ANCESTORS, whose state
    // changes independently of (hwnd, width, height) -- SetParent re-parents
    // them, and a fresh placeholder is 1x1 with clipsToBounds set.
    //
    // Skipping the call left those ancestors at 1x1 and 0x0 while the child's
    // own Metal view was correctly sized, visible and full of content. A
    // clipping ancestor of zero size clips all of it away, so the popup showed
    // as a blank rectangle. Measured on a dropdown:
    //   metal view        150x264 hidden=0
    //   ancestor VtrPixelView 1x1 clips=1
    //   ancestor VtrPixelView 0x0 clips=1
    //
    // The round trip it saved was worth at most 15 ms per second of
    // main-thread blocking (VITREO_MAIN_HOPS). Not worth a window that never
    // draws. Cache a function only on state that covers ALL of its effects.
    __block bool result = false;
    vtr_run_on_main_sync(^{
        const auto control = m_controls.find(hwnd);
        if (control == m_controls.end() || control->second == nullptr) return;
        NSView* child_view = (__bridge NSView*)control->second;
        NSView* parent_view = [child_view superview];
        const NSRect frame = [child_view frame];
        const bool parent_was_flipped =
            parent_view == nil || [parent_view isFlipped];
        const auto saved_origin = m_hosted_peer_origins.find(hwnd);
        const CGFloat win32_top =
            saved_origin != m_hosted_peer_origins.end()
                ? static_cast<CGFloat>(saved_origin->second.second)
                : (parent_was_flipped
                       ? frame.origin.y
                       : [parent_view bounds].size.height - frame.origin.y -
                             frame.size.height);

        // The guest can SetParent the foreign compositor child
        // beneath an intermediate browser-owned HWND while both are still 1x1
        // placeholders. Growing only the leaf leaves it clipped to one pixel
        // by that intermediate NSView. Expand placeholder ancestors as part
        // of the same presentation host, but never resize a real HWND view.
        auto parent_link = m_hosted_peer_parents.find(hwnd);
        HWND placeholder_hwnd = parent_link != m_hosted_peer_parents.end()
            ? parent_link->second
            : NULL_HWND;
        for (NSView* placeholder = parent_view;
             placeholder != nil && [placeholder superview] != nil &&
                 placeholder_hwnd != NULL_HWND;) {
            const NSRect placeholder_frame = [placeholder frame];
            const auto placeholder_control =
                m_controls.find(placeholder_hwnd);
            if (placeholder_control == m_controls.end() ||
                placeholder_control->second !=
                    (__bridge void*)placeholder) {
                break;
            }
            const bool was_present_sized =
                m_hosted_peer_present_sized.find(placeholder_hwnd) !=
                    m_hosted_peer_present_sized.end();
            if ((placeholder_frame.size.width > 1.0 &&
                 placeholder_frame.size.height > 1.0) &&
                !was_present_sized) {
                // The browser-owned compositor container can be created at
                // the first probe surface size before its 1x1 GPU peer is
                // attached. Exact agreement with that first Present is the
                // evidence that this ancestor is presentation-sized; retain
                // that ownership so a later real surface may replace it.
                if (placeholder_frame.size.width !=
                        static_cast<CGFloat>(width) ||
                    placeholder_frame.size.height !=
                        static_cast<CGFloat>(height)) {
                    break;
                }
                m_hosted_peer_present_sized.insert(placeholder_hwnd);
            }
            NSView* container = [placeholder superview];
            const auto placeholder_origin =
                m_hosted_peer_origins.find(placeholder_hwnd);
            CGFloat x = placeholder_origin != m_hosted_peer_origins.end()
                ? static_cast<CGFloat>(placeholder_origin->second.first)
                : placeholder_frame.origin.x;
            CGFloat y = placeholder_frame.origin.y;
            if (![container isFlipped]) {
                const CGFloat container_h = [container bounds].size.height;
                const CGFloat placeholder_win32_top =
                    placeholder_origin != m_hosted_peer_origins.end()
                        ? static_cast<CGFloat>(
                              placeholder_origin->second.second)
                        : container_h - placeholder_frame.origin.y -
                              placeholder_frame.size.height;
                y = container_h - placeholder_win32_top -
                    static_cast<CGFloat>(height);
            }
            [placeholder setFrame:NSMakeRect(
                x, y, static_cast<CGFloat>(width),
                static_cast<CGFloat>(height))];
            // Some control containers retain an explicit bounds size when
            // their frame changes. Keep the hosted presentation coordinate
            // space in lockstep or a 1024x768 probe followed by 700x440
            // places the final child at Y=328 and exposes a white strip.
            NSRect bounds = [placeholder bounds];
            bounds.size = NSMakeSize(static_cast<CGFloat>(width),
                                     static_cast<CGFloat>(height));
            [placeholder setBounds:bounds];
            // Now that this host has a real client area, Win32 child clipping
            // applies to it. It was left unclipped while it was a 0x0/1x1
            // placeholder precisely so it could not clip away the surface it
            // exists to host.
            placeholder.clipsToBounds = YES;
            m_hosted_peer_present_sized.insert(placeholder_hwnd);
            const auto next_link =
                m_hosted_peer_parents.find(placeholder_hwnd);
            placeholder_hwnd = next_link != m_hosted_peer_parents.end()
                ? next_link->second
                : NULL_HWND;
            placeholder = container;
        }

        const CGFloat need_w = static_cast<CGFloat>(width);
        const CGFloat need_h = static_cast<CGFloat>(height);
        // Placeholder hosts are 1×1 (SetParent before the first surface) or
        // were previously sized from a composition Present. A HWND that
        // already has a real CreateWindow/SetWindowPos size must keep it —
        // Present does not SetWindowPos (Microsoft Learn DXGI Present).
        const bool present_sized_placeholder =
            m_hosted_peer_present_sized.find(hwnd) !=
            m_hosted_peer_present_sized.end();
        if (frame.size.width > 1.0 && frame.size.height > 1.0 &&
            !present_sized_placeholder &&
            (frame.size.width != need_w || frame.size.height != need_h)) {
            result = true;
            return;
        }
        // Match ensure_hosted_peer_child: Win32 top-left is stable across
        // resize. Cocoa Y-up parents store the bottom edge of a 1×1 host near
        // parent_h-1; keeping that origin while growing height clips the
        // surface out of the parent (clipsToBounds).
        CGFloat cocoa_x = saved_origin != m_hosted_peer_origins.end()
            ? static_cast<CGFloat>(saved_origin->second.first)
            : frame.origin.x;
        CGFloat cocoa_y = frame.origin.y;
        if (parent_view != nil && ![parent_view isFlipped]) {
            const CGFloat parent_h = [parent_view bounds].size.height;
            cocoa_y = parent_h - win32_top - need_h;
        } else {
            cocoa_y = win32_top;
        }
        // A parent resize changes Cocoa's Y-up origin even when this
        // surface's dimensions stay unchanged. Windows child positions stay
        // relative to the parent's upper left (Learn: Window Features,
        // Child Windows / Positioning), so size alone cannot skip layout.
        if (frame.size.width == need_w && frame.size.height == need_h &&
            frame.origin.x == cocoa_x && frame.origin.y == cocoa_y) {
            result = true;
            return;
        }
        [child_view setFrame:NSMakeRect(cocoa_x, cocoa_y, need_w, need_h)];
        if (present_sized_placeholder || frame.size.width <= 1.0 ||
            frame.size.height <= 1.0) {
            m_hosted_peer_present_sized.insert(hwnd);
        }
        auto presentation = m_presentation_views.find(hwnd);
        if (presentation != m_presentation_views.end() &&
            presentation->second != nullptr) {
            NSView* metal = (__bridge NSView*)presentation->second;
            if ([metal superview] == child_view) {
                [metal setFrame:[child_view bounds]];
            }
        }
        result = true;
        std::fprintf(stderr,
                     "[windowing] grew hosted peer child HWND=0x%x "
                     "present=%dx%d (was %.0fx%.0f @%.0f,%.0f -> @%.0f,%.0f) "
                     "parent=%s flipped=%d frame=%.0fx%.0f bounds=%.0fx%.0f\n",
                     hwnd, width, height, frame.size.width, frame.size.height,
                     frame.origin.x, frame.origin.y, cocoa_x, cocoa_y,
                     NSStringFromClass([parent_view class]).UTF8String,
                     parent_view != nil && [parent_view isFlipped],
                     [parent_view frame].size.width,
                     [parent_view frame].size.height,
                     [parent_view bounds].size.width,
                     [parent_view bounds].size.height);
    });
    // Record only what actually reached the view. A failed grow must not be
    // remembered, or the next present would skip a hop that never happened.
    if (result) {
        std::lock_guard<std::mutex> lock(m_presentation_state_mutex);
        m_peer_present_size[hwnd] = {width, height};
    }
    return result;
}

void CocoaBridge::retire_local_host_after_peer_reparent(HWND child_hwnd) {
    vtr_run_on_main_sync(^{
        const auto child = m_controls.find(child_hwnd);
        if (child == m_controls.end() || child->second == nullptr) return;
        NSView* child_view = (__bridge NSView*)child->second;
        NSWindow* host = [child_view window];
        [child_view removeFromSuperview];

        // Drop any local Metal presentation attached to the GPU-side child so
        // Present only composites through the peer-hosted view.
        auto presentation = m_presentation_views.find(child_hwnd);
        if (presentation != m_presentation_views.end() &&
            presentation->second != nullptr) {
            NSView* metal = (__bridge NSView*)presentation->second;
            if ([metal superview] != nil) {
                [metal removeFromSuperview];
            }
            const HWND presented_hwnd = presentation->first;
            m_presentation_views.erase(presentation);
            forget_presentation_view_state(presented_hwnd);
        }
        m_hosted_peer_children.erase(child_hwnd);
        m_hosted_peer_present_sized.erase(child_hwnd);
        m_hosted_peer_parents.erase(child_hwnd);
        m_hosted_peer_origins.erase(child_hwnd);
        forget_common_control(child_hwnd);
        m_controls.erase(child_hwnd);
        m_controls_hidden_by_guest.erase(child_hwnd);

        if (host == nil) return;
        for (const auto& [hwnd, state] : m_windows) {
            if (state.ns_window != host) continue;
            // Only retire a host the guest is not still showing. This runs
            // whenever a GPU-side composition child is re-parented onto a peer
            // host, and the window that *hosted* that child is very often the
            // application's own window. The guest parents its
            // compositing child under the real top-level. Ordering that out
            // hides the app while the guest still believes its window is
            // visible, which is exactly what a launcher looked like: the
            // guest still reported the window visible, with no later hide,
            // and macOS reported the process owning 0 windows.
            //
            // Vitreo must never take a window off screen that the guest has
            // shown; only a placeholder host it created itself is retirable.
            if (state.visible) {
                std::fprintf(stderr,
                             "[windowing] keeping visible host HWND=0x%x after "
                             "peer SetParent of child=0x%x\n",
                             hwnd, child_hwnd);
                break;
            }
            [host orderOut:nil];
            std::fprintf(stderr,
                         "[windowing] orderOut composition host HWND=0x%x "
                         "after peer SetParent of child=0x%x\n",
                         hwnd, child_hwnd);
            break;
        }
    });
}

} // namespace vitreo::windowing
