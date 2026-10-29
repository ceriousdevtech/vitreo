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
        if (it->second.fullscreen) {
            // Guest SetWindowPos/MoveWindow still updates the Win32 logical
            // rectangle above. AppKit owns the Space frame; do not pin a
            // covering display rectangle over it.
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
