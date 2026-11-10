// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_window_geometry.mm
/// @brief Top-level window geometry.
///
/// Win32 and Cocoa rectangle conversion, the window frame, zoom and
/// minimize, window regions, and the screen size the windowing metrics
/// report.
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
// Coordinate Translation
// =============================================================================

Rect CocoaBridge::windows_to_cocoa_rect(const Rect& win_rect,
                                         i32 screen_height) noexcept {
    Rect result;
    result.left  = win_rect.left;
    result.right = win_rect.right;
    i32 height   = win_rect.bottom - win_rect.top;
    result.top   = screen_height - win_rect.top - height;
    result.bottom = result.top + height;
    return result;
}

Rect CocoaBridge::cocoa_to_windows_rect(const Rect& cocoa_rect,
                                         i32 screen_height) noexcept {
    Rect result;
    result.left  = cocoa_rect.left;
    result.right = cocoa_rect.right;
    i32 height   = cocoa_rect.bottom - cocoa_rect.top;
    result.top   = screen_height - cocoa_rect.top - height;
    result.bottom = result.top + height;
    return result;
}

void CocoaBridge::set_window_rgn(HWND hwnd, bool has_rgn, i32 left, i32 top,
                                 i32 right, i32 bottom, bool redraw,
                                 const std::vector<std::array<i32, 4>>& parts) {
    const std::vector<std::array<i32, 4>> rects = parts;
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || !it->second.ns_window) {
            VITREO_TRACE(
                "[cocoa] set_window_rgn hwnd=0x%x skipped (no native window)\n",
                hwnd);
            return;
        }
        auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        NSView* content = [win contentView];
        NativeWindowState& state = it->second;

        // Learn SetWindowRgn: nothing outside the region is displayed, so the
        // window must not paint its own backdrop there. Remember how it looked
        // so removing the region restores it.
        const auto shape_window = [&](bool shaped) {
            if (shaped == state.region_shaped) return;
            if (shaped) {
                state.pre_region_opaque = [win isOpaque];
                state.pre_region_shadow = [win hasShadow];
                state.pre_region_background =
                    (void*)CFBridgingRetain([win backgroundColor]);
                [win setOpaque:NO];
                [win setBackgroundColor:[NSColor clearColor]];
                [win setHasShadow:NO];
            } else {
                [win setOpaque:state.pre_region_opaque ? YES : NO];
                if (state.pre_region_background != nullptr) {
                    [win setBackgroundColor:(NSColor*)CFBridgingRelease(
                                                state.pre_region_background)];
                    state.pre_region_background = nullptr;
                }
                [win setHasShadow:state.pre_region_shadow ? YES : NO];
                if (state.layered_alpha) apply_layered_alpha_appearance(win);
            }
            state.region_shaped = shaped;
        };

        if (!has_rgn) {
            state.empty_window_rgn = false;
            if (content) {
                content.wantsLayer = YES;
                content.layer.mask = nil;
            }
            shape_window(false);
            sync_native_window_visibility(state);
            VITREO_TRACE("[cocoa] set_window_rgn hwnd=0x%x cleared\n", hwnd);
            if (redraw) {
                [win display];
            }
            return;
        }

        const bool empty = (right <= left) || (bottom <= top);
        state.empty_window_rgn = empty;
        if (empty) {
            if (content) {
                content.wantsLayer = YES;
                content.layer.mask = nil;
            }
            [win orderOut:nil];
            VITREO_TRACE(
                "[cocoa] set_window_rgn hwnd=0x%x EMPTY -> orderOut\n",
                hwnd);
            return;
        }

        // The region's rectangles, relative to the window's upper-left
        // (Win32), scaled into Cocoa points the same way create_window does.
        const CGFloat scale = kWin32PixelScale;
        if (content) {
            content.wantsLayer = YES;
            const CGFloat view_h = content.bounds.size.height;
            CGMutablePathRef path = CGPathCreateMutable();
            const auto add = [&](i32 l, i32 t, i32 r, i32 b) {
                CGPathAddRect(path, nullptr,
                              CGRectMake(static_cast<CGFloat>(l) * scale,
                                         view_h - static_cast<CGFloat>(b) * scale,
                                         static_cast<CGFloat>(r - l) * scale,
                                         static_cast<CGFloat>(b - t) * scale));
            };
            if (rects.empty()) {
                add(left, top, right, bottom);
            } else {
                for (const auto& r : rects) add(r[0], r[1], r[2], r[3]);
            }
            CAShapeLayer* mask = [CAShapeLayer layer];
            mask.frame = content.bounds;
            mask.path = path;
            CGPathRelease(path);
            content.layer.mask = mask;
        }
        // A region that is not simply the whole window leaves parts of the
        // window unshown; only then may the window stop being opaque.
        const NSRect frame = content ? content.bounds : NSZeroRect;
        const bool covers_window =
            rects.size() <= 1 && left <= 0 && top <= 0 &&
            static_cast<CGFloat>(right) * scale >= frame.size.width &&
            static_cast<CGFloat>(bottom) * scale >= frame.size.height;
        shape_window(!covers_window);
        VITREO_TRACE(
            "[cocoa] set_window_rgn hwnd=0x%x rect=(%d,%d)-(%d,%d) parts=%zu\n",
            hwnd, left, top, right, bottom, rects.size());
        if (redraw && state.visible) {
            [win display];
        }
    });
}

// Sets a content view's bounds to (0,0,size) and lays its full-cover
// subviews (the guest pixel surface, presentation views) onto them. AppKit's
// autoresizing works from frame-size deltas, so once the bounds are scaled
// against the frame it moves those subviews off the client origin (a fitted
// dialog showed only its top-left corner, near the screen's bottom right).
void CocoaBridge::set_content_bounds(void* ns_view, double width, double height) {
    auto* content = (__bridge NSView*)ns_view;
    if (content == nil) return;
    [content setBounds:NSMakeRect(0, 0, width, height)];
    constexpr NSAutoresizingMaskOptions kFullCover =
        NSViewWidthSizable | NSViewHeightSizable;
    for (NSView* sub in [content subviews]) {
        if (([sub autoresizingMask] & kFullCover) == kFullCover) {
            [sub setFrame:[content bounds]];
        }
    }
}

void CocoaBridge::set_frame(HWND hwnd, const Rect& rect) {
    // An external resize invalidates the cached present size (ADR: see
    // m_peer_present_size).
    note_control_frame_changed(hwnd);
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) return;

    it->second.frame = rect;

    // NSWindow geometry is main-thread-only. A guest thread reaching here via
    // SetWindowPos (dialogs are created on whichever guest thread calls
    // DialogBoxParam) trapped inside AppKit's window coordinator, killing the
    // process with SIGTRAP. vtr_run_on_main_sync runs inline when already on
    // the main thread, so this is safe from either side.
    vtr_run_on_main_sync(^{
    if (it->second.ns_window) {
        auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        sync_borderless_fullscreen(hwnd);
        if (!it->second.fullscreen && !it->second.space_transition &&
            !window_is_space_fullscreen(win) &&
            place_in_exclusive_display(hwnd, win, rect)) {
            return;
        }
        if (it->second.fullscreen || it->second.space_transition ||
            window_is_space_fullscreen(win)) {
            // Guest SetWindowPos/MoveWindow still updates the Win32 logical
            // rectangle above. AppKit owns the frame of a window in a Space
            // (or entering / leaving one); do not pin a guest rectangle over
            // it. That includes a window whose exclusive owner has just let
            // go but which AppKit has not taken out of the Space yet: an
            // exclusive owner re-taking it at once (a game recreating its
            // device) finds it still in the Space, and an 800x600 frame set
            // in between would stay there. The guest rectangle is applied
            // when the Space has been left
            // (on_native_did_exit_space_fullscreen).
            return;
        }
        // rect represents the desired *content* area (Win32 client rect).
        // Use frameRectForContentRect: so Cocoa's own title bar / borders
        // don't shrink the content, matching initWithContentRect: at creation.
        CGFloat scale = kWin32PixelScale;
        CGFloat w = static_cast<CGFloat>(rect.right - rect.left) * scale;
        CGFloat h = static_cast<CGFloat>(rect.bottom - rect.top) * scale;
        // AppKit rejects a zero content rect; keep a 1×1 native backing while
        // Win32 client_rect may remain 0×0 (CreateWindow / GetClientRect).
        if (w < 1.0) w = 1.0;
        if (h < 1.0) h = 1.0;
        CGFloat x = static_cast<CGFloat>(rect.left) * scale;
        // Convert Win32 top-down Y to Cocoa bottom-up Y
        CGFloat screenH = vtr_win32_flip_height();
        CGFloat y = screenH - static_cast<CGFloat>(rect.top) * scale - h;
        NSRect contentRect = NSMakeRect(x, y, w, h);
        NSRect frame = [win frameRectForContentRect:contentRect];
        // Out of an exclusive display: the content view is unscaled again.
        if (NSView* content = [win contentView];
            content != nil &&
            !NSEqualSizes([content bounds].size, [content frame].size)) {
            set_content_bounds((__bridge void*)content, [content frame].size.width,
                               [content frame].size.height);
        }
        // Programmatic guest SetWindowPos/MoveWindow must not feed back through
        // windowDidResize → WM_SIZE → another MoveWindow (a launcher login
        // grew 693→719 that way as Cocoa title chrome was re-applied).
        //
        // Learn window features: a window whose rect intersects no monitor is
        // not on-screen. setFrame:display:YES plus AppKit's constrainFrameRect
        // would clamp a parked dialog onto the display as a chromeless
        // rectangle between page transitions.
        const bool on_monitor = guest_rect_intersects_any_monitor(rect);
        if (auto* vtr = [win isKindOfClass:[VtrWindow class]]
                ? static_cast<VtrWindow*>(win) : nil) {
            vtr.allowOffscreenFrame = !on_monitor;
        }
        set_suppress_resize_notify(true);
        if (!on_monitor || !it->second.visible) {
            [win orderOut:nil];
            [win setFrame:frame display:NO];
        } else {
            [win setFrame:frame display:YES];
        }
        set_suppress_resize_notify(false);
        // Moving a parked wizard dialog from past SM_CXSCREEN onto the
        // primary work area must order it front; parking the other way must
        // order it out so AppKit does not clamp a second blank wizard on-screen.
        sync_native_window_visibility(it->second);
        if (it->second.is_dialog && it->second.owner != NULL_HWND &&
            should_embed_owned_dialog(hwnd, it->second.guest_style,
                                      it->second.owner)) {
            adopt_extra_family_dialog(hwnd);
        }
    }
    });
}

bool CocoaBridge::place_in_exclusive_display(HWND hwnd, void* ns_window,
                                             const Rect& rect) {
    // While a window of this process holds the exclusive display mode, the
    // guest's whole screen is that mode (user32's virtual display, which
    // update_cursor_clip_screen hands us), and it is aspect-fitted onto the
    // Space exactly as the exclusive surface is (aspect_fit_rect, the
    // swap-chain fit). Every other top-level window lives in the same guest
    // screen, so it is placed through the same fit: its native frame is the
    // fitted image of its guest rectangle, and its content view's bounds are
    // its guest client size, so AppKit scales what it draws and the points
    // its events carry by the same factor. A modal dialog the game draws
    // its menu behind (Armada II) then receives the clicks the user aims at
    // the fitted image.
    HWND exclusive = NULL_HWND;
    NSWindow* exclusive_window = nil;
    for (const auto& [other_hwnd, other] : m_windows) {
        if (other_hwnd == hwnd || !other.fullscreen || other.ns_window == nullptr) {
            continue;
        }
        exclusive = other_hwnd;
        exclusive_window = reinterpret_cast<NSWindow*>(other.ns_window);
        break;
    }
    const Rect& screen_rect = m_guest_cursor_clip_screen;
    const i32 guest_w = screen_rect.right - screen_rect.left;
    const i32 guest_h = screen_rect.bottom - screen_rect.top;
    if (exclusive == NULL_HWND || guest_w <= 0 || guest_h <= 0) return false;
    NSScreen* screen = [exclusive_window screen] ?: [NSScreen mainScreen];
    if (screen == nil) return false;
    // Only a guest display mode makes a separate guest screen: a borderless
    // window that merely covers the display (also held in a Space) leaves
    // the guest screen the host's, and windows stay 1:1.
    if (std::lround([screen frame].size.width) == guest_w &&
        std::lround([screen frame].size.height) == guest_h) {
        return false;
    }
    // The guest screen is shown where the exclusive surface is fitted: the
    // exclusive window's content area once its Space has formed. On a display
    // with a camera housing AppKit keeps that area below it (1800x1130 of an
    // 1800x1169 panel), so fitting to the screen frame put windows larger
    // and higher than the image they belong to. Before the Space forms the
    // screen frame stands in; on_native_did_enter_space_fullscreen re-places.
    NSRect host = [screen frame];
    if (window_is_space_fullscreen(exclusive_window)) {
        if (NSView* content = [exclusive_window contentView]; content != nil) {
            const NSRect area = [exclusive_window
                convertRectToScreen:[content convertRect:[content bounds] toView:nil]];
            if (area.size.width >= 1.0 && area.size.height >= 1.0) host = area;
        }
    }
    const auto fit = graphics::aspect_fit_rect(guest_w, guest_h, host.size.width,
                                               host.size.height);
    ExclusiveDisplayPlacement place{};
    if (!exclusive_display_placement(screen_rect, fit.x, fit.y, fit.width,
                                     fit.height, rect, place)) {
        return false;
    }
    const double scale = place.scale;
    const double client_w = place.width / scale;
    const double client_h = place.height / scale;
    // Guest top-down placement, then Cocoa's bottom-up screen space.
    const double bottom_up_y =
        host.origin.y + host.size.height - place.top - place.height;
    auto* win = reinterpret_cast<NSWindow*>(ns_window);
    const NSRect content_rect = NSMakeRect(host.origin.x + place.left, bottom_up_y,
                                           place.width, place.height);
    set_suppress_resize_notify(true);
    [win setFrame:[win frameRectForContentRect:content_rect] display:YES];
    if (NSView* content = [win contentView]; content != nil) {
        set_content_bounds((__bridge void*)content, client_w, client_h);
    }
    set_suppress_resize_notify(false);
    VITREO_TRACE("[cocoa] exclusive-display placement hwnd=0x%x guest=(%d,%d %dx%d) "
                 "host=(%.0f,%.0f %.0fx%.0f) scale=%.3f\n",
                 hwnd, rect.left, rect.top, rect.right - rect.left,
                 rect.bottom - rect.top, content_rect.origin.x, content_rect.origin.y,
                 content_rect.size.width, content_rect.size.height, scale);
    return true;
}

void CocoaBridge::replace_windows_for_exclusive_display(HWND exclusive) {
    // Entering or leaving the exclusive display re-places the process's other
    // top-level windows: into the fitted guest screen, or back to 1:1.
    std::vector<std::pair<HWND, Rect>> others;
    for (const auto& [other_hwnd, other] : m_windows) {
        if (other_hwnd != exclusive && other.ns_window != nullptr && !other.fullscreen) {
            others.emplace_back(other_hwnd, other.frame);
        }
    }
    for (const auto& [other_hwnd, frame] : others) set_frame(other_hwnd, frame);
}

Rect CocoaBridge::zoom_window(HWND hwnd) {
    __block Rect result{};
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || !it->second.ns_window) return;

        auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);

        // Compute the maximized frame: visible screen area (excluding dock/menu bar).
        // Use setFrame:display:animate:NO to avoid the Cocoa zoom animation,
        // which would generate dozens of intermediate resize events.
        NSScreen* screen = [win screen] ?: [NSScreen mainScreen];
        NSRect maxFrame = [screen visibleFrame];
        [win setFrame:maxFrame display:YES animate:NO];

        // Read back the resulting content size
        NSRect content = [win contentRectForFrameRect:[win frame]];
        CGFloat scale = kWin32PixelScale;
        result.left   = static_cast<i32>(content.origin.x / scale);
        result.top    = 0; // Will be computed by caller
        result.right  = result.left + static_cast<i32>(content.size.width / scale);
        result.bottom = static_cast<i32>(content.size.height / scale);

        it->second.frame = result;
    });
    return result;
}

void CocoaBridge::set_minimized(HWND hwnd, bool minimized) {
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || !it->second.ns_window) return;
        it->second.requested_minimized = minimized;
        if (!minimized) it->second.minimize_attempted = false;

        auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        if (minimized) {
            if (![win isMiniaturized]) {
                // Win32 minimizes any top-level window, minimize box or not;
                // AppKit ignores -miniaturize: without the miniaturizable
                // bit. A window whose chrome shows no minimize button gets
                // the bit only for this request: dropping it again while the
                // window is in the Dock keeps it there and leaves its title
                // bar unchanged when it comes back (both measured).
                const NSUInteger mask = [win styleMask];
                const bool lend = (mask & NSWindowStyleMaskMiniaturizable) == 0;
                if (lend) [win setStyleMask:mask | NSWindowStyleMaskMiniaturizable];
                [win miniaturize:nil];
                if (lend) [win setStyleMask:mask];
            }
        } else if ([win isMiniaturized]) {
            [win deminiaturize:nil];
        }
        // Do not treat an initially minimized HWND as a refused native
        // request before its initial show/minimize has actually been tried.
        if (auto current = m_windows.find(hwnd);
            current != m_windows.end() && current->second.ns_window == win &&
            current->second.requested_minimized) {
            current->second.minimize_attempted = true;
        }
    });
}

void CocoaBridge::get_screen_size(i32& out_w, i32& out_h) const {
    // Windowing metrics stay in Cocoa points so CreateWindow / mouse
    // mapping stay 1:1. D3D9 / EnumDisplaySettings use get_screen_pixel_size.
    // SM_CXSCREEN is the primary monitor: [NSScreen screens][0], not the
    // focus-following mainScreen.
    NSScreen* screen = [[NSScreen screens] firstObject];
    if (screen) {
        NSRect frame = [screen frame];
        out_w = static_cast<i32>(frame.size.width);
        out_h = static_cast<i32>(frame.size.height);
    } else {
        out_w = 1920;
        out_h = 1080;
    }
}

void CocoaBridge::get_screen_pixel_size(i32& out_w, i32& out_h) const {
    vtr_nsscreen_backing_pixels([[NSScreen screens] firstObject], out_w, out_h);
}

} // namespace vitreo::windowing
