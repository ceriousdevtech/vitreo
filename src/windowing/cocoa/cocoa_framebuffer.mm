// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_framebuffer.mm
/// @brief Guest pixel surfaces: update, capture and blit.
///
/// GDI output reaches the screen as a VtrPixelView per top-level HWND. These
/// functions create that view, copy guest pixels into it, read a window's
/// pixels back, and blit pending pixels during live resize.
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

// Helper: find (or create) the VtrPixelView owned by one top-level HWND.
// Handles both legacy (VtrPixelView IS the contentView) and new
// (VtrPixelView is a subview behind child controls) cases.  Never discover a
// surface by scanning arbitrary subviews: those views can belong to child
// HWNDs, and sharing one lets a child presentation replace its parent's frame.
// dialog_mode: wizard pages use native NSControls. Aspect-fit letterboxing
// after grow produced thick black bars and stole button hits.
static VtrPixelView* find_or_create_pixel_view(NSView* content,
                                               VtrPixelView* owned_pixel_view,
                                               bool dialog_mode = false,
                                               bool aspect_fit = false) {
    const auto configure = ^(VtrPixelView* pixel_view) {
        if (dialog_mode) {
            pixel_view.scalesFramebuffer = NO;
            pixel_view.passesThroughEvents = YES;
            pixel_view.layer.contentsGravity = kCAGravityResize;
            pixel_view.layer.backgroundColor =
                [NSColor clearColor].CGColor;
        } else if (aspect_fit) {
            pixel_view.scalesFramebuffer = YES;
            pixel_view.layer.contentsGravity = kCAGravityResizeAspect;
        } else {
            // Ordinary GDI: 1:1 top-left, matching BitBlt into the client.
            // Aspect-fit is exclusive-fullscreen only — otherwise a smaller
            // framebuffer in a large popup becomes a letterboxed blue/black
            // plate (skinned installer hosts).
            pixel_view.scalesFramebuffer = NO;
            pixel_view.layer.contentsGravity = kCAGravityTopLeft;
        }
    };
    if ([content isKindOfClass:[VtrPixelView class]]) {
        auto* pixel_view = (VtrPixelView*)content;
        configure(pixel_view);
        return pixel_view;  // legacy
    }
    if (owned_pixel_view != nil && [owned_pixel_view superview] == content) {
        configure(owned_pixel_view);
        return owned_pixel_view;
    }
    // First time: create and insert behind all child controls.
    VtrPixelView* pv = [[VtrPixelView alloc] initWithFrame:content.bounds];
    configure(pv);
    pv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:pv positioned:NSWindowBelow relativeTo:nil];
    return pv;
}

namespace vitreo::windowing {

// =============================================================================
// Framebuffer Pixel Update
// =============================================================================

bool CocoaBridge::capture_window_rgba(HWND hwnd, std::vector<u8>& out_rgba,
                                      i32& out_width, i32& out_height) {
    __block bool ok = false;
    __block std::vector<u8> pixels;
    __block i32 bw = 0;
    __block i32 bh = 0;

    vtr_run_on_main_sync(^{
        @autoreleasepool {
            NSView* view = nil;
            auto wit = m_windows.find(hwnd);
            if (wit != m_windows.end() && wit->second.ns_window) {
                NSWindow* window =
                    reinterpret_cast<NSWindow*>(wit->second.ns_window);
                view = [window contentView];
            } else {
                auto cit = m_controls.find(hwnd);
                if (cit != m_controls.end() && cit->second) {
                    view = (__bridge NSView*)cit->second;
                }
            }
            if (!view) {
                return;
            }

            const NSRect bounds = [view bounds];
            bw = static_cast<i32>(std::ceil(bounds.size.width));
            bh = static_cast<i32>(std::ceil(bounds.size.height));
            if (bw <= 0 || bh <= 0) {
                return;
            }

            // SoftBitmap / BitBlt / update_window_pixels use RGBA byte order.
            pixels.assign(static_cast<size_t>(bw) * static_cast<size_t>(bh) * 4u,
                          0xFF);
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            if (!cs) {
                return;
            }
            CGContextRef ctx = CGBitmapContextCreate(
                pixels.data(), static_cast<size_t>(bw), static_cast<size_t>(bh),
                8, static_cast<size_t>(bw) * 4u, cs,
                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(cs);
            if (!ctx) {
                return;
            }

            // flipped:YES matches the content view's top-left origin. Combined
            // with the row reverse below, the snapshot is top-down SoftBitmap.
            NSGraphicsContext* gctx =
                [NSGraphicsContext graphicsContextWithCGContext:ctx
                                                        flipped:YES];
            if (!gctx) {
                CGContextRelease(ctx);
                return;
            }
            [NSGraphicsContext saveGraphicsState];
            [NSGraphicsContext setCurrentContext:gctx];
            [view displayRectIgnoringOpacity:bounds inContext:gctx];
            [NSGraphicsContext restoreGraphicsState];
            CGContextRelease(ctx);

            // CGBitmapContext's first row is the bottom of the context.
            // flipped:YES draws the view's top at the context top, which is
            // the last buffer row. SoftBitmap and screen-DC BitBlt are
            // top-down; without this reverse, Next in a wizard blits the
            // parked page upside down until the live HWND is shown.
            const size_t stride = static_cast<size_t>(bw) * 4u;
            std::vector<u8> scratch(stride);
            for (i32 y = 0; y < bh / 2; ++y) {
                u8* top = pixels.data() + static_cast<size_t>(y) * stride;
                u8* bot =
                    pixels.data() + static_cast<size_t>(bh - 1 - y) * stride;
                std::memcpy(scratch.data(), top, stride);
                std::memcpy(top, bot, stride);
                std::memcpy(bot, scratch.data(), stride);
            }

            ok = true;
            VITREO_TRACE("[cocoa] capture_window_rgba hwnd=0x%x -> %dx%d\n",
                         hwnd, bw, bh);
        }
    });

    if (!ok) {
        return false;
    }
    out_rgba = std::move(pixels);
    out_width = bw;
    out_height = bh;
    return true;
}

void CocoaBridge::set_control_owns_input(HWND hwnd) {
    vtr_run_on_main_sync(^{
        if (!m_input_owning_controls.insert(hwnd).second) return;
        auto it = m_guest_pixel_views.find(hwnd);
        if (it != m_guest_pixel_views.end() && it->second != nullptr) {
            ((__bridge VtrPixelView*)it->second).passesThroughEvents = NO;
        }
    });
}

void CocoaBridge::update_window_pixels(HWND hwnd, const u8* rgba, i32 width,
                                       i32 height,
                                       bool above_native_children) {
    // GDI presentation normally enters here on Vitreo's guest execution
    // thread, not an AppKit-managed thread. Autoreleased frame copies created
    // below would therefore accumulate for the lifetime of the process unless
    // this boundary supplies and drains its own pool.
    @autoreleasepool {
    if (!rgba || width <= 0 || height <= 0) return;

    auto it = m_windows.find(hwnd);
    if (it == m_windows.end() || !it->second.ns_window) {
        // Not a top-level window. It may be an app-painted child window or a
        // native standard control that the guest subclassed and paints itself.
        // Keep the native view for control behavior. Dialog/container
        // framebuffers are page backgrounds and must sit behind their native
        // child controls; an app-painted individual control owns its whole
        // client area, so its framebuffer sits above the native fallback.
        auto ctrl_it = m_controls.find(hwnd);
        if (ctrl_it != m_controls.end() && ctrl_it->second) {
            size_t clen = static_cast<size_t>(width) * height * 4;
            NSData* px = [NSData dataWithBytes:rgba length:clen];
            i32 w = width;
            i32 h = height;
            NSView* child_view = (__bridge NSView*)ctrl_it->second;
            // Blank white framebuffers from measure-only GetDC must not cover
            // native text/button controls (a path static whose text is "C:").
            // An empty STATIC that the guest paints is not labeled — it still
            // needs a pixel surface.
            NSString* native_text = nil;
            if ([child_view isKindOfClass:[NSTextField class]]) {
                native_text = [(NSTextField*)child_view stringValue];
            } else if ([child_view isKindOfClass:[NSButton class]]) {
                native_text = [(NSButton*)child_view title];
            }
            const bool native_labeled =
                native_text != nil && [native_text length] > 0;
            if (native_labeled) {
                bool all_white = true;
                const size_t npx = static_cast<size_t>(width) * height;
                for (size_t i = 0; i < npx; ++i) {
                    const u8* p = rgba + i * 4;
                    if (p[0] != 0xFF || p[1] != 0xFF || p[2] != 0xFF) {
                        all_white = false;
                        break;
                    }
                }
                if (all_white) {
                    vtr_run_on_main_sync(^{
                        auto pixel_it = m_guest_pixel_views.find(hwnd);
                        if (pixel_it != m_guest_pixel_views.end() &&
                            pixel_it->second) {
                            NSView* overlay =
                                (__bridge NSView*)pixel_it->second;
                            [overlay removeFromSuperview];
                            m_guest_pixel_views.erase(pixel_it);
                            VITREO_TRACE(
                                "[cocoa] update_window_pixels: skipped blank "
                                "overlay on native control hwnd=0x%x\n",
                                hwnd);
                        }
                    });
                    return;
                }
            }
            vtr_run_on_main_sync(^{
                VtrPixelView* pixel_view = nil;
                NSView* overlay_host = child_view;
                NSRect overlay_frame = [child_view bounds];
                if (above_native_children && [child_view window] != nil) {
                    // A foreign-process DirectComposition child is represented
                    // by pixels delivered to its owning parent HWND. Custom
                    // guest controls are layer-hosting VtrPixelViews; AppKit
                    // does not reliably composite an added subview above that
                    // manually-owned layer tree. Host the transported surface
                    // at the containing window's composition root instead and
                    // convert the target's client bounds into that coordinate
                    // system. This also places the surface above native
                    // descendants, matching the foreign child-window z-order.
                    NSView* content = [[child_view window] contentView];
                    if (content != nil) {
                        overlay_host = content;
                        overlay_frame =
                            [child_view convertRect:[child_view bounds]
                                             toView:content];
                        // Learn: WS_CHILD drawing is clipped to the parent
                        // client area. An unclipped content-root overlay for a
                        // negatively positioned compositor child paints over
                        // custom title chrome (minimize/close).
                        content.clipsToBounds = YES;
                        overlay_frame =
                            NSIntersectionRect(overlay_frame, [content bounds]);
                    }
                }
                if (above_native_children && NSIsEmptyRect(overlay_frame)) {
                    return;
                }
                if (!above_native_children &&
                    [child_view isKindOfClass:[VtrPixelView class]]) {
                    pixel_view = (VtrPixelView*)child_view;
                } else {
                    auto pixel_it = m_guest_pixel_views.find(hwnd);
                    if (pixel_it != m_guest_pixel_views.end() &&
                        pixel_it->second != nullptr) {
                        pixel_view = (__bridge VtrPixelView*)pixel_it->second;
                    }
                    if (pixel_view == nil) {
                        pixel_view = [[VtrPixelView alloc]
                            initWithFrame:overlay_frame];
                        pixel_view.autoresizingMask =
                            NSViewWidthSizable | NSViewHeightSizable;
                        const NSWindowOrderingMode order =
                            above_native_children ||
                                ![child_view
                                    isKindOfClass:[VtrContainerView class]]
                            ? NSWindowAbove
                            : NSWindowBelow;
                        [overlay_host addSubview:pixel_view
                                      positioned:order
                                      relativeTo:nil];
                        m_guest_pixel_views[hwnd] =
                            (__bridge void*)pixel_view;
                        VITREO_TRACE(
                            "[cocoa] update_window_pixels: child hwnd=0x%x "
                            "created guest-painted overlay\n", hwnd);
                    }
                }
                // Hit testing must follow the same z-order as presentation.
                // A transported foreign-child surface placed above native
                // descendants owns that visible area; allowing events to pass
                // through instead selects an obscured descendant and applies
                // its unrelated client coordinate space. Background/dialog
                // pixel layers remain transparent to native child controls.
                if (pixel_view != child_view) {
                    pixel_view.passesThroughEvents =
                        above_native_children ||
                                m_input_owning_controls.contains(hwnd)
                            ? NO
                            : YES;
                }
                if (above_native_children && pixel_view != child_view) {
                    if ([pixel_view superview] != overlay_host) {
                        [pixel_view removeFromSuperview];
                    }
                    [overlay_host addSubview:pixel_view
                                  positioned:NSWindowAbove
                                  relativeTo:nil];
                }
                // Keep the native view sized to the guest framebuffer so a
                // MoveWindow(640→1280) + later flush cannot leave a clipped
                // TopLeft blit inside a stale 640×480 frame.
                if (above_native_children && pixel_view != child_view) {
                    [pixel_view setFrame:overlay_frame];
                } else {
                    NSRect bounds = [pixel_view bounds];
                    if (std::fabs(bounds.size.width - w) > 0.5 ||
                        std::fabs(bounds.size.height - h) > 0.5) {
                        NSRect fr = [pixel_view frame];
                        fr.size.width = static_cast<CGFloat>(w);
                        fr.size.height = static_cast<CGFloat>(h);
                        [pixel_view setFrame:fr];
                    }
                }
                pixel_view.compositesAlpha = YES;
                [pixel_view updatePixels:(const uint8_t*)[px bytes]
                                   width:w
                                  height:h];
            });
        }
        return;
    }

    size_t data_len = static_cast<size_t>(width) * height * 4;

    // During live resize the main thread is inside AppKit's tracking loop.
    // CFRunLoopPerformBlock blocks (used by vtr_run_on_main_sync/async) are
    // NOT processed during tracking — they pile up until the mouse is
    // released.  Instead, store the pixels in a shared buffer and let the
    // live-resize NSTimer (which IS fired during tracking) blit them.
    if (m_in_live_resize.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(m_pending_px_mutex);
        auto& pp = m_pending_pixels[hwnd];
        pp.data.assign(rgba, rgba + data_len);
        pp.width  = width;
        pp.height = height;
        pp.dirty  = true;
        return;
    }

    // Normal (non-resize) path: sync dispatch so the frame is visible
    // before the guest thread continues.
    NSData* pixelData = [NSData dataWithBytes:rgba length:data_len];
    i32 w = width;
    i32 h = height;
    NSWindow* window = reinterpret_cast<NSWindow*>(it->second.ns_window);
    const bool dialog_mode = it->second.is_dialog;
    vtr_run_on_main_sync(^{
        NSView* content = [window contentView];
        VtrPixelView* owned_pixel_view = nil;
        auto pixel_it = m_guest_pixel_views.find(hwnd);
        if (pixel_it != m_guest_pixel_views.end() &&
            pixel_it->second != nullptr) {
            owned_pixel_view = (__bridge VtrPixelView*)pixel_it->second;
        }
        VtrPixelView* pixelView =
            find_or_create_pixel_view(content, owned_pixel_view, dialog_mode,
                                      it->second.fullscreen);
        m_guest_pixel_views[hwnd] = (__bridge void*)pixelView;
        auto state_it = m_windows.find(hwnd);
        pixelView.compositesAlpha =
            state_it != m_windows.end() && state_it->second.layered_alpha ? YES : NO;
        [pixelView updatePixels:(const uint8_t*)[pixelData bytes] width:w height:h];
    });
    }
}

bool CocoaBridge::has_guest_pixel_surface(HWND hwnd) const {
    __block bool found = false;
    vtr_run_on_main_sync(^{
        auto direct_it = m_controls.find(hwnd);
        if (direct_it != m_controls.end() && direct_it->second != nullptr) {
            NSView* view = (__bridge NSView*)direct_it->second;
            if ([view isKindOfClass:[VtrPixelView class]]) {
                found = true;
                return;
            }
        }
        auto overlay_it = m_guest_pixel_views.find(hwnd);
        found = overlay_it != m_guest_pixel_views.end() &&
                overlay_it->second != nullptr;
    });
    return found;
}

bool CocoaBridge::foreign_presentation_is_control_local(HWND hwnd) const {
    __block bool local = false;
    vtr_run_on_main_sync(^{
        auto control = m_controls.find(hwnd);
        auto presentation = m_presentation_views.find(hwnd);
        if (control == m_controls.end() || control->second == nullptr ||
            presentation == m_presentation_views.end() ||
            presentation->second == nullptr) {
            return;
        }
        NSView* target = (__bridge NSView*)control->second;
        NSView* view = (__bridge NSView*)presentation->second;
        local = [view isKindOfClass:[VtrMetalView class]] &&
                [view superview] == target && target.clipsToBounds;
    });
    return local;
}

bool CocoaBridge::guest_pixel_surface_is_behind_native_children(
    HWND hwnd) const {
    __block bool behind = false;
    vtr_run_on_main_sync(^{
        auto control_it = m_controls.find(hwnd);
        auto pixel_it = m_guest_pixel_views.find(hwnd);
        if (control_it == m_controls.end() ||
            pixel_it == m_guest_pixel_views.end() ||
            control_it->second == nullptr ||
            pixel_it->second == nullptr) {
            return;
        }

        NSView* container = (__bridge NSView*)control_it->second;
        NSView* pixel_view = (__bridge NSView*)pixel_it->second;
        NSArray<NSView*>* subviews = [container subviews];
        const NSUInteger pixel_index =
            [subviews indexOfObjectIdenticalTo:pixel_view];
        behind = pixel_index != NSNotFound &&
                 [subviews count] > 1 &&
                 pixel_index == 0;
    });
    return behind;
}

// =============================================================================
// Blit Pending Pixels (main-thread timer callback for live resize)
// =============================================================================

void CocoaBridge::blit_pending_pixels(HWND hwnd) {
    // Read pending pixels under lock
    std::vector<u8> px_copy;
    i32 w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(m_pending_px_mutex);
        auto it = m_pending_pixels.find(hwnd);
        if (it == m_pending_pixels.end() || !it->second.dirty) return;
        px_copy = it->second.data;
        w = it->second.width;
        h = it->second.height;
        it->second.dirty = false;
    }

    auto wit = m_windows.find(hwnd);
    if (wit == m_windows.end() || !wit->second.ns_window) return;

    NSWindow* window = reinterpret_cast<NSWindow*>(wit->second.ns_window);
    NSView* content = [window contentView];
    VtrPixelView* owned_pixel_view = nil;
    auto pixel_it = m_guest_pixel_views.find(hwnd);
    if (pixel_it != m_guest_pixel_views.end() &&
        pixel_it->second != nullptr) {
        owned_pixel_view = (__bridge VtrPixelView*)pixel_it->second;
    }
    VtrPixelView* pixelView =
        find_or_create_pixel_view(content, owned_pixel_view,
                                  wit->second.is_dialog,
                                  wit->second.fullscreen);
    m_guest_pixel_views[hwnd] = (__bridge void*)pixelView;
    [pixelView updatePixels:px_copy.data() width:w height:h];
}

} // namespace vitreo::windowing
