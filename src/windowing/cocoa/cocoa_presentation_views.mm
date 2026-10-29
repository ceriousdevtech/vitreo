// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_presentation_views.mm
/// @brief The views guest presentation draws into.
///
/// The Metal-backed view a swap chain presents to, the view a foreign
/// process presents to, their hidden state and timestamps, and the debug
/// descriptions of both.
///
/// Split out of cocoa_bridge.mm, which was 7,624 lines. These are CocoaBridge
/// methods defined in a separate translation unit; the class is unchanged.

#include "windowing/cocoa/cocoa_perf_overlay.h"
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

bool CocoaBridge::presentation_view_is_hidden(HWND hwnd) const {
    __block bool hidden = true;
    vtr_run_on_main_sync(^{
      @autoreleasepool {
        auto it = m_presentation_views.find(hwnd);
        if (it == m_presentation_views.end() || it->second == nullptr) {
            auto detached = m_detached_presentation_views.find(hwnd);
            if (detached == m_detached_presentation_views.end() ||
                detached->second == nullptr) {
                return;
            }
            hidden = ((__bridge NSView*)detached->second).hidden;
            return;
        }
        hidden = ((__bridge NSView*)it->second).hidden;
      }
    });
    return hidden;
}

void CocoaBridge::forget_presentation_view_state(HWND hwnd) {
    std::lock_guard<std::mutex> lock(m_presentation_state_mutex);
    m_presentation_view_hidden.erase(hwnd);
    m_peer_present_size.erase(hwnd);
}

void CocoaBridge::note_control_frame_changed(HWND hwnd) {
    std::lock_guard<std::mutex> lock(m_presentation_state_mutex);
    m_peer_present_size.erase(hwnd);
}

void* CocoaBridge::native_presentation_view(HWND hwnd) {
    __block void* result = nullptr;
    vtr_run_on_main_sync(^{
        // A swap chain replaces this HWND's CPU-painted client surface, but
        // must remain below its child controls and diagnostic overlays.
        auto place_surface = [&](NSView* host, NSView* surface) {
            NSView* pixels = nil;
            auto owned = m_guest_pixel_views.find(hwnd);
            if (owned != m_guest_pixel_views.end())
                pixels = (__bridge NSView*)owned->second;
            if (pixels && pixels.superview == host) {
                [host addSubview:surface positioned:NSWindowAbove relativeTo:pixels];
            } else if (surface.superview != host) {
                [host addSubview:surface positioned:NSWindowBelow relativeTo:nil];
            }
        };
        auto it = m_windows.find(hwnd);
        if (it != m_windows.end() && it->second.ns_window) {
            NSWindow* window =
                reinterpret_cast<NSWindow*>(it->second.ns_window);
            NSView* content = [window contentView];
            for (NSView* subview in [content subviews]) {
                if ([subview isKindOfClass:[VtrMetalView class]]) {
                    place_surface(subview.superview, subview);
                    result = (__bridge void*)subview;
                    sync_borderless_fullscreen(hwnd);
                    return;
                }
            }
            VtrMetalView* presentation_view = [[VtrMetalView alloc]
                initWithFrame:[content bounds]];
            presentation_view.autoresizingMask =
                NSViewWidthSizable | NSViewHeightSizable;
            place_surface(content, presentation_view);
            result = (__bridge void*)presentation_view;
            vtr_perf_overlay_attach((__bridge void*)content);
            sync_borderless_fullscreen(hwnd);
            if (!m_cursor_hidden && m_custom_ns_cursor) {
                [(__bridge NSCursor*)m_custom_ns_cursor set];
                vtr_invalidate_cursor_rects_recursive(window, content);
            }
            return;
        }

        // DirectComposition targets may be child HWNDs. Child windows are
        // represented by NSViews rather than NSWindows, so host the Metal
        // presentation view inside the existing child view. The compositor
        // subsequently applies the target's topmost/bottommost ordering.
        auto control = m_controls.find(hwnd);
        if (control != m_controls.end() && control->second != nullptr) {
            NSView* container = (__bridge NSView*)control->second;
            if ([container isKindOfClass:[VtrMetalView class]]) {
                result = (__bridge void*)container;
                return;
            }
            for (NSView* subview in [container subviews]) {
                if ([subview isKindOfClass:[VtrMetalView class]]) {
                    place_surface(subview.superview, subview);
                    result = (__bridge void*)subview;
                    return;
                }
            }
            VtrMetalView* presentation_view = [[VtrMetalView alloc]
                initWithFrame:[container bounds]];
            presentation_view.autoresizingMask =
                NSViewWidthSizable | NSViewHeightSizable;
            place_surface(container, presentation_view);
            result = (__bridge void*)presentation_view;
            std::fprintf(stderr,
                         "[windowing] Metal presentation view attached to "
                         "child HWND=0x%x\n",
                         hwnd);
            return;
        }

        // A genuinely foreign HWND has no local NSWindow or child NSView.
        // Keep a detached layer only as a rendering/readback target; the
        // foreign-window presenter transports its pixels to the owner.
        auto detached = m_detached_presentation_views.find(hwnd);
        if (detached != m_detached_presentation_views.end() &&
            detached->second != nullptr) {
            result = detached->second;
            return;
        }
        VtrMetalView* orphan = [[VtrMetalView alloc]
            initWithFrame:NSMakeRect(0, 0, 800, 600)];
        [orphan setWantsLayer:YES];
        void* retained = (__bridge_retained void*)orphan;
        // A presentation view is CREATED HIDDEN, and views are detached and
        // recreated for the same HWND during a run. Cached state describes the
        // OLD view, so it must be dropped here or the next
        // set_presentation_view_hidden(hwnd, false) skips the un-hide as
        // redundant and the window never appears.
        forget_presentation_view_state(hwnd);
        m_detached_presentation_views[hwnd] = retained;
        result = retained;
        std::fprintf(stderr,
                     "[windowing] detached Metal presentation view for "
                     "foreign HWND=0x%x (OOP swap chain)\n",
                     hwnd);
    });
    // NOTE: a DETACHED orphan is deliberately NOT recorded here.
    //
    // m_presentation_views is what set_presentation_view_hidden acts on, and
    // the orphan is a readback target that is not in any view hierarchy. If it
    // were recorded, then for a hosted peer child -- which has BOTH an orphan
    // (created by the renderer's path) and a real hosted view -- whichever
    // call ran last would decide which view "the" presentation view is. Show
    // the orphan and the real view, created hidden, stays hidden: the popup's
    // pixels arrive but nothing visible displays them, which is a white
    // rectangle where a dropdown should be.
    bool result_is_orphan = false;
    if (result != nullptr) {
        const auto orphan = m_detached_presentation_views.find(hwnd);
        result_is_orphan = orphan != m_detached_presentation_views.end() &&
                           orphan->second == result;
    }
    if (result != nullptr && !result_is_orphan) {
        forget_presentation_view_state(hwnd);
        m_presentation_views[hwnd] = result;
        debug_describe_presentation_view(hwnd, "native-view-created");
    }
    return result;
}

void* CocoaBridge::native_foreign_presentation_view(HWND hwnd) {
    __block void* result = nullptr;
    vtr_run_on_main_sync(^{
        // CreateSwapChainForComposition is not HWND-bound (Learn). The
        // swap chain becomes visible only after DComp SetContent on a
        // visual, typically a child HWND. A top-level m_windows hit is
        // the GDI/CPU framebuffer owner; covering it with a Metal overlay
        // and tearing down that pixel view hides StretchDIBits presents.
        const auto control = m_controls.find(hwnd);
        if (control == m_controls.end() || control->second == nullptr) {
            return;
        }
        NSView* target = (__bridge NSView*)control->second;
        NSWindow* window = [target window];
        if (window == nil) return;

        // Learn: a WS_CHILD HWND is clipped to its parent's client area.
        // The guest often positions the DirectComposition
        // child with a negative Y so the surface sits under custom title
        // chrome. Hosting the Metal
        // view on the window contentView at the unclipped converted frame
        // paints over that chrome; parenting it to the child control and
        // enabling ancestor clipsToBounds matches Win32 clipping.
        //
        // But ONLY clip to an ancestor that has a real size. These hosts are
        // created as 1x1 (SetParent before the first surface) or 0x0
        // placeholders and are sized later, from a composition Present. A
        // zero-size view with clipsToBounds set clips ALL of its descendants
        // away, permanently, however correct they are -- so enabling it here
        // unconditionally is a race: if the ancestors happen to be sized
        // before the presentation view is created the popup draws, and if not
        // it is invisible. That is why a dropdown rendered correctly once
        // and blank every other time. Measured on a live one:
        //   metal view              132x165 hidden=0 alpha=1.00
        //   ancestor VtrPixelView   132x165 clips=1   (at y=-329)
        //   ancestor VtrPixelView     0x0   clips=1   <- everything dies here
        //
        // A placeholder with no size does not represent a Win32 client area,
        // so clipping to it is not the Win32 rule -- it is an artifact of our
        // lazy sizing. grow_hosted_peer_child_to_present_size turns clipping
        // on as it gives each host its real size.
        // The bar is not "has any size" -- it is "is at least as large as the
        // surface it hosts". These intermediate VtrPixelViews are OUR hosting
        // artifacts, not Win32 windows with client areas, and they are sized
        // lazily from composition Presents. A first attempt allowed anything
        // bigger than 1x1 to clip, and a host still at 2x33 then reduced a
        // 150x264 dropdown to an invisible sliver.
        //
        // A genuine Win32 parent that is smaller than its child does clip it,
        // but for these compositor hosts a too-small ancestor means "not sized
        // yet", and the two failure modes are not comparable: not clipping
        // costs a little overdraw, clipping to a placeholder loses the window
        // entirely.
        const NSSize hosted = [target bounds].size;
        for (NSView* ancestor = target; ancestor != nil;
             ancestor = [ancestor superview]) {
            const NSSize ancestor_size = [ancestor bounds].size;
            ancestor.clipsToBounds =
                (ancestor_size.width >= hosted.width &&
                 ancestor_size.height >= hosted.height)
                    ? YES : NO;
        }

        auto existing = m_presentation_views.find(hwnd);
        VtrMetalView* view = nil;
        if (existing != m_presentation_views.end() &&
            existing->second != nullptr) {
            NSView* candidate = (__bridge NSView*)existing->second;
            if ([candidate isKindOfClass:[VtrMetalView class]]) {
                view = static_cast<VtrMetalView*>(candidate);
            }
        }
        if (view == nil) {
            view = [[VtrMetalView alloc] initWithFrame:[target bounds]];
            view.autoresizingMask =
                NSViewWidthSizable | NSViewHeightSizable;
            view.wantsLayer = YES;
            // Clear until the first real frame is presented — a default
            // opaque Metal layer is a solid dark plate over sibling views.
            view.layer.backgroundColor = NSColor.clearColor.CGColor;
            view.hidden = YES;
            [target addSubview:view
                     positioned:NSWindowAbove
                     relativeTo:nil];
            result = (__bridge void*)view;
            forget_presentation_view_state(hwnd);
            m_presentation_views[hwnd] = result;
        } else {
            if ([view superview] != target) {
                [view removeFromSuperview];
                [target addSubview:view
                         positioned:NSWindowAbove
                         relativeTo:nil];
            }
            result = (__bridge void*)view;
        }
        [view setFrame:[target bounds]];

        // GPU content replaces CPU fallback for this child visual only —
        // never tear down a sibling/parent guest pixel surface.
        auto pixels = m_guest_pixel_views.find(hwnd);
        if (pixels != m_guest_pixel_views.end() && pixels->second != nullptr) {
            NSView* pixel_view = (__bridge NSView*)pixels->second;
            [pixel_view removeFromSuperview];
            m_guest_pixel_views.erase(pixels);
        }
    });
    return result;
}

double present_view_timestamp() {
    // Wall clock, so lines from the producing GPU process and the showing
    // browser process can be ordered against each other.
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const double ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count()) /
        1000.0;
    return std::fmod(ms / 1000.0, 100000.0);
}

void CocoaBridge::debug_describe_presentation_view(HWND hwnd,
                                                   const char* tag) {
    static const bool enabled =
        std::getenv("VITREO_LOG_PRESENT_VIEW") != nullptr;
    if (!enabled) return;
    vtr_run_on_main_sync(^{
        auto it = m_presentation_views.find(hwnd);
        if (it == m_presentation_views.end() || it->second == nullptr) {
            std::fprintf(stderr,
                         "[present-view] t=%.3f %s hwnd=0x%x NO PRESENTATION VIEW\n",
                         present_view_timestamp(), tag,
                         static_cast<unsigned>(hwnd));
            return;
        }
        NSView* view = (__bridge NSView*)it->second;
        NSRect f = [view frame];
        // What the screen can actually show for this view: the Metal layer's
        // drawable size and whether it holds contents.
        CALayer* layer = view.layer;
        CGSize drawable = CGSizeZero;
        if ([layer isKindOfClass:[CAMetalLayer class]]) {
            drawable = ((CAMetalLayer*)layer).drawableSize;
        }
        CALayer* first_sub = layer.sublayers.count > 0 ? layer.sublayers[0] : nil;
        std::fprintf(stderr,
            "[present-view] t=%.3f %s hwnd=0x%x frame=%.0f,%.0f %.0fx%.0f hidden=%d "
            "alpha=%.2f layer=%p window=%p visible=%d layer_class=%s "
            "contents=%d drawable=%.0fx%.0f opaque=%d sublayers=%lu "
            "sub0=%s sub0_contents=%d win_opaque=%d win_alpha=%.2f\n",
            present_view_timestamp(), tag, static_cast<unsigned>(hwnd),
            f.origin.x, f.origin.y,
            f.size.width, f.size.height, view.hidden ? 1 : 0,
            (double)view.alphaValue, (void*)view.layer,
            (void*)[view window], [[view window] isVisible] ? 1 : 0,
            layer ? [[[layer class] description] UTF8String] : "nil",
            layer.contents != nil ? 1 : 0, drawable.width, drawable.height,
            layer.opaque ? 1 : 0, (unsigned long)layer.sublayers.count,
            first_sub ? [[[first_sub class] description] UTF8String] : "-",
            first_sub.contents != nil ? 1 : 0,
            [[view window] isOpaque] ? 1 : 0,
            (double)[[view window] alphaValue]);
        int depth = 0;
        for (NSView* a = view; a != nil && depth < 6;
             a = [a superview], ++depth) {
            NSRect af = [a frame];
            std::fprintf(stderr,
                "[present-view]    view-chain[%d] %s frame=%.0f,%.0f %.0fx%.0f "
                "hidden=%d clips=%d\n",
                depth, [[[a class] description] UTF8String],
                af.origin.x, af.origin.y, af.size.width, af.size.height,
                a.hidden ? 1 : 0, a.clipsToBounds ? 1 : 0);
            NSArray<NSView*>* order = [[a superview] subviews];
            const NSUInteger own_index = [order indexOfObject:a];
            for (NSView* sibling in order) {
                if (sibling == a) continue;
                const NSRect sf = sibling.frame;
                // above=1: composited over view-chain[depth] (AppKit subview
                // order is bottom to top).
                std::fprintf(stderr,
                    "[present-view]    sibling[%d] %s frame=%.0f,%.0f %.0fx%.0f "
                    "hidden=%d alpha=%.2f above=%d\n", depth,
                    [[[sibling class] description] UTF8String],
                    sf.origin.x, sf.origin.y, sf.size.width, sf.size.height,
                    sibling.hidden ? 1 : 0, (double)sibling.alphaValue,
                    [order indexOfObject:sibling] > own_index ? 1 : 0);
            }
        }
    });
}

void CocoaBridge::debug_describe_window_presentation_views(void* ns_window,
                                                          const char* tag) {
    static const bool enabled =
        std::getenv("VITREO_LOG_PRESENT_VIEW") != nullptr;
    if (!enabled || ns_window == nullptr) return;
    NSWindow* window = (__bridge NSWindow*)ns_window;
    __block std::vector<HWND> hosted;
    vtr_run_on_main_sync(^{
        for (const auto& [surface_hwnd, surface_view] : m_presentation_views) {
            if (surface_view != nullptr &&
                [(__bridge NSView*)surface_view window] == window) {
                hosted.push_back(surface_hwnd);
            }
        }
    });
    for (HWND surface : hosted) debug_describe_presentation_view(surface, tag);
}

bool CocoaBridge::set_presentation_view_hidden(HWND hwnd, bool hidden) {
    // Skip the main-thread round trip when nothing would change. See the note
    // on m_presentation_view_hidden: this is called once per child per present.
    {
        std::lock_guard<std::mutex> lock(m_presentation_state_mutex);
        const auto seen = m_presentation_view_hidden.find(hwnd);
        if (seen != m_presentation_view_hidden.end() && seen->second == hidden) {
            return false;
        }
        m_presentation_view_hidden[hwnd] = hidden;
    }
    vtr_run_on_main_sync(^{
        auto it = m_presentation_views.find(hwnd);
        if (it == m_presentation_views.end() || it->second == nullptr) return;
        NSView* view = (__bridge NSView*)it->second;
        view.hidden = hidden ? YES : NO;
    });
    return true;
}

} // namespace vitreo::windowing
