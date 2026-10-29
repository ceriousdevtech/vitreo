// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_child_windows.mm
/// @brief Child-window views: parent, frame and z-order.
///
/// A Win32 child window is an NSView inside its parent's view. These
/// functions reparent it, place and size it, answer where it is, and keep
/// sibling z-order in step with SetWindowPos.
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

bool CocoaBridge::set_control_parent(HWND child_hwnd, HWND parent_hwnd) {
    __block bool result = false;
    vtr_run_on_main_sync(^{
        const auto child = m_controls.find(child_hwnd);
        if (child == m_controls.end() || child->second == nullptr) return;
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
        NSView* child_view = (__bridge NSView*)child->second;
        [child_view removeFromSuperview];
        [parent_view addSubview:child_view];
        result = true;
    });
    return result;
}

bool CocoaBridge::has_control_view(HWND hwnd) const {
    __block bool found = false;
    vtr_run_on_main_sync(^{
        const auto it = m_controls.find(hwnd);
        found = it != m_controls.end() && it->second != nullptr;
    });
    return found;
}

HWND CocoaBridge::resolve_client_mouse_target(HWND top_level, i32 client_x,
                                              i32 client_y,
                                              HWND* containing_hwnd) const {
    return retarget_presentation_mouse(top_level, client_x, client_y,
                                       containing_hwnd);
}

bool CocoaBridge::control_view_frame(HWND hwnd, i32& out_x, i32& out_y,
                                     i32& out_w, i32& out_h) const {
    __block bool result = false;
    __block i32 x = 0;
    __block i32 y = 0;
    __block i32 width = 0;
    __block i32 height = 0;
    vtr_run_on_main_sync(^{
        const auto control = m_controls.find(hwnd);
        if (control == m_controls.end() || control->second == nullptr) return;
        NSView* view = (__bridge NSView*)control->second;
        const NSRect frame = [view frame];
        x = static_cast<i32>(frame.origin.x);
        y = static_cast<i32>(frame.origin.y);
        width = static_cast<i32>(frame.size.width);
        height = static_cast<i32>(frame.size.height);
        result = true;
    });
    if (result) {
        out_x = x;
        out_y = y;
        out_w = width;
        out_h = height;
    }
    return result;
}

bool CocoaBridge::control_view_size(HWND hwnd, i32& out_w, i32& out_h) const {
    out_w = 0;
    out_h = 0;
    __block bool result = false;
    __block i32 width = 0;
    __block i32 height = 0;
    vtr_run_on_main_sync(^{
        const auto control = m_controls.find(hwnd);
        if (control == m_controls.end() || control->second == nullptr) return;
        NSView* view = (__bridge NSView*)control->second;
        const NSRect frame = [view frame];
        width = static_cast<i32>(std::lround(frame.size.width));
        height = static_cast<i32>(std::lround(frame.size.height));
        result = true;
    });
    if (result) {
        out_w = width;
        out_h = height;
    }
    return result;
}

bool CocoaBridge::control_view_is_above(HWND hwnd, HWND sibling) const {
    __block bool result = false;
    vtr_run_on_main_sync(^{
        const auto left = m_controls.find(hwnd);
        const auto right = m_controls.find(sibling);
        if (left == m_controls.end() || left->second == nullptr ||
            right == m_controls.end() || right->second == nullptr) {
            return;
        }
        NSView* view = (__bridge NSView*)left->second;
        NSView* other = (__bridge NSView*)right->second;
        NSView* parent = [view superview];
        if (parent == nil || [other superview] != parent) return;
        const NSInteger view_index = [[parent subviews] indexOfObject:view];
        const NSInteger other_index = [[parent subviews] indexOfObject:other];
        result = view_index != NSNotFound && other_index != NSNotFound &&
                 view_index > other_index;
    });
    return result;
}

usize CocoaBridge::sibling_control_views(HWND hwnd) const {
    __block usize count = 0;
    vtr_run_on_main_sync(^{
        const auto found = m_controls.find(hwnd);
        if (found == m_controls.end() || found->second == nullptr) return;
        NSView* parent = [(__bridge NSView*)found->second superview];
        if (parent == nil) return;
        for (NSView* view in [parent subviews]) {
            if ([view isKindOfClass:[NSTextField class]] || [view isKindOfClass:[NSButton class]]) ++count;
        }
    });
    return count;
}

bool CocoaBridge::set_control_z_order(HWND hwnd, HWND insert_after) {
    // NOT cached on "already raised". A sibling raised later puts this view
    // back under it, so a cache keyed on what THIS window last asked for would
    // skip a legitimate re-raise -- and with many hosted children each raising
    // every frame, they invalidate each other anyway, so it would buy nothing.
    // The per-frame cost is removed at the CALLER instead: the peer-present
    // path now raises only when the view actually becomes visible, which is
    // the state change the raise exists to correct.
    __block bool result = false;
    vtr_run_on_main_sync(^{
        const auto child = m_controls.find(hwnd);
        if (child == m_controls.end() || child->second == nullptr) return;
        NSView* view = (__bridge NSView*)child->second;
        NSView* parent = [view superview];
        if (parent == nil) return;
        if (insert_after == HWND_TOP || insert_after == HWND_TOPMOST) {
            [parent addSubview:view positioned:NSWindowAbove relativeTo:nil];
            restack_transparent_children((__bridge void*)parent);
            result = true;
            return;
        }
        if (insert_after == HWND_BOTTOM || insert_after == HWND_NOTOPMOST) {
            [parent addSubview:view positioned:NSWindowBelow relativeTo:nil];
            restack_transparent_children((__bridge void*)parent);
            result = true;
            return;
        }
        NSView* after_view = nil;
        const auto after_control = m_controls.find(insert_after);
        if (after_control != m_controls.end() &&
            after_control->second != nullptr) {
            after_view = (__bridge NSView*)after_control->second;
        }
        if (after_view != nil && [after_view superview] == parent) {
            [parent addSubview:view positioned:NSWindowAbove
                    relativeTo:after_view];
        } else {
            [parent addSubview:view positioned:NSWindowAbove relativeTo:nil];
        }
        restack_transparent_children((__bridge void*)parent);
        result = true;
    });
    return result;
}

void CocoaBridge::restack_transparent_children(void* parent_ptr) {
    NSView* parent = (__bridge NSView*)parent_ptr;
    if (parent == nil || m_transparent_children.empty()) return;
    // Which of the parent's subviews are child windows, and which of those
    // are WS_EX_TRANSPARENT. Other subviews (the parent's own painted
    // surface, presentation layers) keep their place.
    std::unordered_map<void*, bool> child_is_transparent;
    for (const auto& [hwnd, view_ptr] : m_controls) {
        if (view_ptr == nullptr) continue;
        if ([(__bridge NSView*)view_ptr superview] != parent) continue;
        child_is_transparent[view_ptr] =
            m_transparent_children.count(hwnd) != 0;
    }
    NSView* lowest_opaque = nil;
    std::vector<NSView*> transparent_above;  // bottom to top
    for (NSView* subview in [parent subviews]) {  // AppKit order: bottom first
        const auto it = child_is_transparent.find((__bridge void*)subview);
        if (it == child_is_transparent.end()) continue;
        if (!it->second) {
            if (lowest_opaque == nil) lowest_opaque = subview;
        } else if (lowest_opaque != nil) {
            transparent_above.push_back(subview);
        }
    }
    // Each lands directly below the lowest opaque child, so processing them
    // bottom to top keeps their relative order.
    for (NSView* view : transparent_above) {
        [parent addSubview:view positioned:NSWindowBelow
                relativeTo:lowest_opaque];
    }
}

bool CocoaBridge::set_control_frame(HWND ctrl_hwnd, const Rect& rect) {
    // An external resize invalidates the cached present size (ADR: see
    // m_peer_present_size).
    note_control_frame_changed(ctrl_hwnd);
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return false;
    }

    NSView* view = (__bridge NSView*)it->second;


    void (^updateBlock)(void) = ^{
        NSView* parent = [view superview];
        CGFloat parent_h = parent ? [parent bounds].size.height : 0.0;

        CGFloat x = static_cast<CGFloat>(rect.left);
        CGFloat w = static_cast<CGFloat>(rect.right - rect.left);
        CGFloat h = static_cast<CGFloat>(rect.bottom - rect.top);
        CGFloat y;

        if (!parent || [parent isFlipped]) {
            y = static_cast<CGFloat>(rect.top);
        } else {
            // Unflipped (Y-up) parent, including a not-yet-sized one: the
            // origin is bottom-relative, so a later parent resize can be
            // compensated by a uniform shift (see below).
            y = parent_h - static_cast<CGFloat>(rect.top) - h;
        }

        const CGFloat old_h = [view bounds].size.height;
        [view setFrame:NSMakeRect(x, y, w, h)];
        [view setNeedsDisplay:YES];

        // Win32 child positions are relative to the parent's client top-left;
        // resizing a window never moves its children. An unflipped view keeps
        // subview origins bottom-relative, so a height change would slide every
        // Win32 child by the delta (Inno Setup's scaled wizard header drew
        // ~74 px low). Shift only Win32 child views; AppKit's own subviews
        // (scrollers, borders) keep their autoresizing behaviour.
        const CGFloat delta = [view bounds].size.height - old_h;
        if (delta != 0.0 && ![view isFlipped]) {
            for (const auto& [child_hwnd, child_ptr] : m_controls) {
                (void)child_hwnd;
                NSView* child = (__bridge NSView*)child_ptr;
                if (child == view || [child superview] != view) continue;
                NSPoint origin = [child frame].origin;
                origin.y += delta;
                [child setFrameOrigin:origin];
            }
        }
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        vtr_run_on_main_sync(updateBlock);
    }

    return true;
}

} // namespace vitreo::windowing
