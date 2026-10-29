// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_pointer_mapping.mm
/// @brief Mapping points between AppKit windows and guest surfaces.
///
/// Window points to guest points through the presented surface -- a
/// CAMetalLayer drawable or a scaled GDI framebuffer -- and back, the
/// viewport arithmetic behind both directions, and scroll-wheel
/// translation.
///
/// Split out of cocoa_bridge.mm, which was 7,624 lines. The code is
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

bool map_native_mouse_point(NSWindow* window, NSPoint window_point,
                                   vitreo::i32& guest_x, vitreo::i32& guest_y,
                                   bool clamp_outside) {
    (void)clamp_outside;
    NSView* content = [window contentView];

    // Host events stay in top-level view space so hit-testing can see child
    // origins. post_client_mouse_message() maps through the presented
    // surface exactly once in retarget_presentation_mouse.
    const NSPoint content_point =
        [content convertPoint:window_point fromView:nil];
    guest_x = static_cast<vitreo::i32>(content_point.x);
    guest_y = static_cast<vitreo::i32>(content_point.y);
    return true;
}

namespace {
bool surface_geometry(NSView* view, vitreo::graphics::PresentationRect& fit,
                      vitreo::u32& width, vitreo::u32& height) {
    width = height = 0;
    if ([view isKindOfClass:[VtrMetalView class]]) {
        for (CALayer* layer in view.layer.sublayers) {
            if (![layer isKindOfClass:[CAMetalLayer class]]) continue;
            const CGSize size = ((CAMetalLayer*)layer).drawableSize;
            width = static_cast<vitreo::u32>(size.width);
            height = static_cast<vitreo::u32>(size.height);
            break;
        }
    } else if ([view isKindOfClass:[VtrPixelView class]]) {
        VtrPixelView* pixel = (VtrPixelView*)view;
        if (pixel.scalesFramebuffer && pixel.framebufferWidth > 0 && pixel.framebufferHeight > 0) {
            width = static_cast<vitreo::u32>(pixel.framebufferWidth);
            height = static_cast<vitreo::u32>(pixel.framebufferHeight);
        }
    }
    if (width == 0 || height == 0) return false;
    fit = vitreo::graphics::aspect_fit_rect(width, height,
        view.bounds.size.width, view.bounds.size.height);
    return true;
}
NSPoint surface_origin(NSView* view, NSView* content) {
    const NSRect bounds = view.bounds;
    const NSPoint origin = NSMakePoint(NSMinX(bounds),
        view.isFlipped ? NSMinY(bounds) : NSMaxY(bounds));
    return vtr_win32_point_in_view(content, [content convertPoint:origin fromView:view]);
}
}

bool map_native_point_to_guest_surface(NSWindow* window, NSPoint window_point,
                                       bool clamp_outside,
                                       vitreo::i32& guest_x,
                                       vitreo::i32& guest_y) {
    NSView* content = window.contentView;
    const NSPoint point = [content convertPoint:window_point fromView:nil];
    NSPoint hit_point = point;
    if (clamp_outside && content.bounds.size.width > 0 && content.bounds.size.height > 0) {
        const NSRect bounds = content.bounds;
        hit_point.x = std::clamp(hit_point.x, NSMinX(bounds), std::nextafter(NSMaxX(bounds), NSMinX(bounds)));
        hit_point.y = std::clamp(hit_point.y, NSMinY(bounds), std::nextafter(NSMaxY(bounds), NSMinY(bounds)));
    }
    // Only the nearest surface on the hit branch owns this point. A sibling
    // background (or a Metal ancestor behind a nearer pixel view) cannot
    // change the foreground child's coordinates by changing its drawable.
    for (NSView* view = [content hitTest:[content convertPoint:hit_point toView:content.superview]]; view != nil && view != content; view = view.superview) {
        if (![view isKindOfClass:[VtrMetalView class]] &&
            ![view isKindOfClass:[VtrPixelView class]]) continue;
        vitreo::graphics::PresentationRect fit{};
        vitreo::u32 width = 0, height = 0;
        if (!surface_geometry(view, fit, width, height)) break;
        const NSPoint local = vtr_win32_point_in_view(view,
            [view convertPoint:window_point fromView:nil]);
        vitreo::windowing::Point mapped{};
        if (!vitreo::windowing::CocoaBridge::map_presentation_point(local.x, local.y,
            fit.x, fit.y, fit.width, fit.height, width, height, mapped, clamp_outside)) return false;
        const NSPoint origin = surface_origin(view, content);
        guest_x = mapped.x + static_cast<vitreo::i32>(origin.x);
        guest_y = mapped.y + static_cast<vitreo::i32>(origin.y);
        return true;
    }
    const NSPoint fallback = vtr_win32_point_in_view(content, point);
    guest_x = static_cast<vitreo::i32>(fallback.x);
    guest_y = static_cast<vitreo::i32>(fallback.y);
    return true;
}

bool map_guest_surface_point_to_native(NSWindow* window, vitreo::i32 guest_x,
                                       vitreo::i32 guest_y, NSPoint& window_point,
                                       bool require_surface) {
    NSView* content = window.contentView;
    // Surface guest rectangles can overlap after scaling. Search frontmost
    // descendants first, and accept only candidates which the forward hit
    // mapping agrees with. This rejects hidden/occluded background inverses.
    const auto visit = [&](auto&& self, NSView* view) -> bool {
        if (view.hidden) return false;
        for (NSView* child in view.subviews.reverseObjectEnumerator)
            if (self(self, child)) return true;
        vitreo::graphics::PresentationRect fit{};
        vitreo::u32 width = 0, height = 0;
        if (!surface_geometry(view, fit, width, height)) {
            if (require_surface) return false;
            const NSPoint candidate = [content convertPoint:vtr_win32_point_in_view(content,
                NSMakePoint(guest_x, guest_y)) toView:nil];
            const NSPoint local = [view convertPoint:candidate fromView:nil];
            vitreo::i32 check_x = 0, check_y = 0;
            if (!NSPointInRect(local, view.bounds) ||
                !map_native_point_to_guest_surface(window, candidate, false, check_x, check_y) ||
                check_x != guest_x || check_y != guest_y) return false;
            window_point = candidate;
            return true;
        }
        const NSPoint origin = surface_origin(view, content);
        const auto local_x = guest_x - static_cast<vitreo::i32>(origin.x);
        const auto local_y = guest_y - static_cast<vitreo::i32>(origin.y);
        if (local_x < 0 || local_y < 0 || local_x >= static_cast<vitreo::i32>(width) ||
            local_y >= static_cast<vitreo::i32>(height)) return false;
        double x = 0, y = 0;
        if (!vitreo::windowing::CocoaBridge::unmap_presentation_point(local_x, local_y,
            fit.x, fit.y, fit.width, fit.height, width, height, x, y)) return false;
        const NSPoint candidate = [view convertPoint:vtr_win32_point_in_view(view, NSMakePoint(x, y)) toView:nil];
        vitreo::i32 check_x = 0, check_y = 0;
        if (!map_native_point_to_guest_surface(window, candidate, false, check_x, check_y) ||
            check_x != guest_x || check_y != guest_y) return false;
        window_point = candidate;
        return true;
    };
    if (visit(visit, content)) return true;
    if (require_surface) return false;
    // Ordinary native controls and points outside the client use desktop
    // coordinates directly, with no unrelated presentation transform.
    window_point = [content convertPoint:vtr_win32_point_in_view(content,
        NSMakePoint(guest_x, guest_y)) toView:nil];
    return true;
}

bool vitreo::windowing::CocoaBridge::map_presentation_point(
    double point_x, double point_y,
    double viewport_x, double viewport_y,
    double viewport_width, double viewport_height,
    u32 surface_width, u32 surface_height, Point& mapped,
    bool clamp_outside) noexcept {
    if (viewport_width <= 0 || viewport_height <= 0 ||
        surface_width == 0 || surface_height == 0) {
        return false;
    }

    if (clamp_outside) {
        // Keep the clamped sample strictly inside the half-open viewport so
        // the right/bottom edges resolve to surface_width-1 / height-1.
        const double max_x = viewport_x + viewport_width -
                             std::numeric_limits<double>::epsilon();
        const double max_y = viewport_y + viewport_height -
                             std::numeric_limits<double>::epsilon();
        if (point_x < viewport_x) point_x = viewport_x;
        if (point_y < viewport_y) point_y = viewport_y;
        if (point_x > max_x) point_x = max_x;
        if (point_y > max_y) point_y = max_y;
    } else if (point_x < viewport_x || point_y < viewport_y ||
               point_x >= viewport_x + viewport_width ||
               point_y >= viewport_y + viewport_height) {
        return false;
    }

    mapped.x = static_cast<i32>((point_x - viewport_x) * surface_width /
                                viewport_width);
    mapped.y = static_cast<i32>((point_y - viewport_y) * surface_height /
                                viewport_height);
    if (mapped.x < 0) mapped.x = 0;
    if (mapped.y < 0) mapped.y = 0;
    if (mapped.x >= static_cast<i32>(surface_width)) {
        mapped.x = static_cast<i32>(surface_width) - 1;
    }
    if (mapped.y >= static_cast<i32>(surface_height)) {
        mapped.y = static_cast<i32>(surface_height) - 1;
    }
    return true;
}

bool vitreo::windowing::CocoaBridge::unmap_presentation_point(
    i32 guest_x, i32 guest_y,
    double viewport_x, double viewport_y,
    double viewport_width, double viewport_height,
    u32 surface_width, u32 surface_height,
    double& point_x, double& point_y) noexcept {
    if (viewport_width <= 0 || viewport_height <= 0 ||
        surface_width == 0 || surface_height == 0) {
        return false;
    }
    // Aim at the centre of the guest pixel so a round-trip through
    // map_presentation_point lands back on the same pixel rather than on the
    // boundary, where truncation could pick the neighbour.
    point_x = viewport_x +
              (static_cast<double>(guest_x) + 0.5) * viewport_width /
                  static_cast<double>(surface_width);
    point_y = viewport_y +
              (static_cast<double>(guest_y) + 0.5) * viewport_height /
                  static_cast<double>(surface_height);
    return true;
}

bool vitreo::windowing::CocoaBridge::guest_clip_to_viewport_bounds(
    i32 left, i32 top, i32 right, i32 bottom, double viewport_x,
    double viewport_y, double viewport_width, double viewport_height,
    u32 surface_width, u32 surface_height, double& out_x_min,
    double& out_y_min, double& out_x_max, double& out_y_max) noexcept {
    if (right <= left || bottom <= top) return false;
    // ClipCursor right/bottom are exclusive in the usual GetClientRect sense;
    // map the inclusive interior corners through the fitted viewport.
    const i32 x1 = right - 1;
    const i32 y1 = bottom - 1;
    double corners[4][2];
    const i32 xs[2] = {left, x1};
    const i32 ys[2] = {top, y1};
    int n = 0;
    for (i32 y : ys) {
        for (i32 x : xs) {
            if (!unmap_presentation_point(x, y, viewport_x, viewport_y,
                                          viewport_width, viewport_height,
                                          surface_width, surface_height,
                                          corners[n][0], corners[n][1])) {
                return false;
            }
            ++n;
        }
    }
    out_x_min = out_x_max = corners[0][0];
    out_y_min = out_y_max = corners[0][1];
    for (int i = 1; i < 4; ++i) {
        out_x_min = std::min(out_x_min, corners[i][0]);
        out_x_max = std::max(out_x_max, corners[i][0]);
        out_y_min = std::min(out_y_min, corners[i][1]);
        out_y_max = std::max(out_y_max, corners[i][1]);
    }
    // Expand half a guest-pixel so the clip covers the full edge pixels that
    // unmap aimed at the centre of.
    const double half_x =
        0.5 * viewport_width / static_cast<double>(surface_width);
    const double half_y =
        0.5 * viewport_height / static_cast<double>(surface_height);
    out_x_min -= half_x;
    out_x_max += half_x;
    out_y_min -= half_y;
    out_y_max += half_y;
    return out_x_max > out_x_min && out_y_max > out_y_min;
}

namespace vitreo::windowing {

// =============================================================================
// Private Helpers
// =============================================================================

void CocoaBridge::translate_scroll_wheel(void* ns_event, MessageQueue& queue) {
    @autoreleasepool {
        NSEvent* event = (__bridge NSEvent*)ns_event;
        if (!event) return;

        NSWindow* win = [event window];
        if (!win) win = [NSApp keyWindow];
        if (vtr_native_text_editor_focused(win)) return;

        HWND target = find_hwnd_for_window(win);
        if (target == NULL_HWND) {
            target = active_hwnd();
        }
        const HWND top_level = target;
        // Native macOS wheel input follows the hovered surface, matching
        // Windows MOUSEWHEEL_ROUTING_MOUSE_POS (wheel_routing_probe, both
        // bitnesses and axes). Keyboard focus must not redirect it to a
        // hidden focus sink. Use the click path so a peer compositor routes
        // to its nearest locally dispatchable ancestor, not its transport HWND.
        i32 client_x = 0, client_y = 0;
        map_native_mouse_point(win, [event locationInWindow], client_x, client_y, false);
        target = resolve_client_mouse_target(top_level, client_x, client_y);
        if (diag::input_diag()) {
            std::fprintf(stderr,
                "[input] wheel-native window=%ld top=0x%x target=0x%x precise=%d delta=(%.3f,%.3f)\n",
                static_cast<long>([win windowNumber]), top_level, target,
                static_cast<int>([event hasPreciseScrollingDeltas]),
                static_cast<double>([event scrollingDeltaX]),
                static_cast<double>([event scrollingDeltaY]));
        }
        if (target == NULL_HWND) return;

        // Apple NSEvent supplies fractional lines/pixels. Microsoft Learn:
        // WM_MOUSEWHEEL accepts divisions of WHEEL_DELTA, leaving scroll
        // granularity to the receiving application. Rounding to whole lines
        // here discarded fine wheel movements and delayed trackpad input.
        i32 wheel_y = m_wheel_delta_y.translate(target,
            [event scrollingDeltaY], [event hasPreciseScrollingDeltas]);
        i32 wheel_x = m_wheel_delta_x.translate(target,
            [event scrollingDeltaX], [event hasPreciseScrollingDeltas]);
        if (wheel_y == 0 && wheel_x == 0) return;
        // Windows reports the device's wheel through raw input and
        // DirectInput as well as WM_MOUSEWHEEL; clients that read the mouse
        // only from raw input otherwise never see the wheel.
        if (m_native_mouse_wheel_callback) {
            if (wheel_y != 0) m_native_mouse_wheel_callback(wheel_y, false, target);
            if (wheel_x != 0) m_native_mouse_wheel_callback(wheel_x, true, target);
        }

        // WM_MOUSEWHEEL lParam is cursor position in *screen* coordinates.
        i32 screen_x = 0;
        i32 screen_y = 0;
        // Use the event's location, not the cursor position when a delayed
        // queue drain happens. Convert into the guest desktop coordinate space.
        const auto top = m_windows.find(top_level);
        if (top == m_windows.end()) return;
        if (!map_native_point_to_guest_surface(win, [event locationInWindow],
                top->second.fullscreen, screen_x, screen_y)) {
            map_native_mouse_point(win, [event locationInWindow], screen_x, screen_y, false);
        }
        if (!top->second.fullscreen) {
            screen_x += top->second.frame.left;
            screen_y += top->second.frame.top;
        }

        const u64 mk = static_cast<u64>(m_mouse_buttons & 0x3)
                     | mouse_modifier_flags(event);
        const u32 lparam =
            ((static_cast<u32>(screen_y) & 0xFFFF) << 16)
            | (static_cast<u32>(screen_x) & 0xFFFF);
        // Snapshot only host-owned scalar state at native routing, not later
        // dispatch (where guest focus may already have changed). Parameters
        // remain raw as well as decoded: Learn WM_MOUSEWHEEL specifies signed
        // 16-bit screen coordinates and wheel delta, on both guest bitnesses.
        const bool trace_route = diag::input_diag();
        const HWND route_focus = trace_route ? keyboard_focus_hwnd() : NULL_HWND;
        const auto trace_wheel = [&](const MSG& msg) {
            if (!trace_route) return;
            std::fprintf(stderr,
                "[input] wheel-route stage=native-post pid=%d monotonic_ns=%llu "
                "top=0x%x target=0x%x focus=0x%x msg=0x%x "
                "wparam=0x%llx lparam=0x%llx delta=%d keys=0x%x screen=(%d,%d)\n",
                static_cast<int>(::getpid()),
                static_cast<unsigned long long>(input_latency::now_ns()),
                top_level, msg.hwnd, route_focus, msg.message,
                static_cast<unsigned long long>(msg.wParam),
                static_cast<unsigned long long>(msg.lParam),
                static_cast<int>(static_cast<i16>(msg.wParam >> 16)),
                static_cast<unsigned>(msg.wParam & 0xffff),
                static_cast<int>(static_cast<i16>(msg.lParam)),
                static_cast<int>(static_cast<i16>(msg.lParam >> 16)));
        };

        while (wheel_y != 0) {
            const i32 delta = std::clamp(wheel_y, -32760, 32760);
            wheel_y -= delta;
            MSG msg{};
            msg.hwnd = target;
            msg.message = WM::MOUSEWHEEL;
            msg.wParam = mk
                | (static_cast<u64>(static_cast<u16>(delta)) << 16);
            msg.lParam = lparam;
            msg.pt = {screen_x, screen_y};
            msg.time = static_cast<u32>(continuous_tick_count_ms());
            trace_wheel(msg);
            queue.post_input(msg);
        }
        while (wheel_x != 0) {
            const i32 delta = std::clamp(wheel_x, -32760, 32760);
            wheel_x -= delta;
            MSG msg{};
            msg.hwnd = target;
            msg.message = WM::MOUSEHWHEEL;
            msg.wParam = mk
                | (static_cast<u64>(static_cast<u16>(delta)) << 16);
            msg.lParam = lparam;
            msg.pt = {screen_x, screen_y};
            msg.time = static_cast<u32>(continuous_tick_count_ms());
            trace_wheel(msg);
            queue.post_input(msg);
        }
    }
}

} // namespace vitreo::windowing
