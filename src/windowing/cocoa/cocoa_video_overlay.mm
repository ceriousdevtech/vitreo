// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_video_overlay.mm
/// @brief Video overlay child windows.
///
/// A video overlay is a borderless child window placed over a window's
/// Metal layer and filled with guest RGBA frames. The helpers find that
/// layer and its screen frame; the CocoaBridge functions show, update and
/// hide the overlay.
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

static CAMetalLayer* vtr_find_metal_layer_in_layer(CALayer* layer) {
    if (layer == nil) return nil;
    if ([layer isKindOfClass:[CAMetalLayer class]]) {
        return (CAMetalLayer*)layer;
    }
    for (CALayer* sub in layer.sublayers) {
        CAMetalLayer* found = vtr_find_metal_layer_in_layer(sub);
        if (found != nil) return found;
    }
    return nil;
}

static CAMetalLayer* vtr_find_metal_layer(NSView* view) {
    if (view == nil) return nil;
    CAMetalLayer* found = vtr_find_metal_layer_in_layer(view.layer);
    if (found != nil) return found;
    for (NSView* sub in view.subviews) {
        found = vtr_find_metal_layer(sub);
        if (found != nil) return found;
    }
    return nil;
}

static NSView* vtr_metal_host_view(NSView* content, CAMetalLayer* metal) {
    if (metal != nil && metal.superlayer != nil &&
        [metal.superlayer.delegate isKindOfClass:[NSView class]]) {
        return (NSView*)metal.superlayer.delegate;
    }
    return content;
}

static NSRect vtr_video_overlay_screen_frame(NSWindow* window, NSView* content,
                                             CAMetalLayer* metal) {
    NSView* host = vtr_metal_host_view(content, metal);
    NSRect view_rect = host.bounds;
    if (metal != nil) {
        view_rect = NSRectFromCGRect(metal.frame);
    }
    const NSRect window_rect = [host convertRect:view_rect toView:nil];
    return [window convertRectToScreen:window_rect];
}

void vtr_close_video_overlay_window(void* ptr) {
    if (ptr == nullptr) return;
    NSWindow* overlay = (__bridge_transfer NSWindow*)ptr;
    NSWindow* parent = [overlay parentWindow];
    if (parent != nil) {
        [parent removeChildWindow:overlay];
    }
    [overlay orderOut:nil];
    [overlay close];
}

static void vtr_set_layer_rgba(CALayer* layer, const uint8_t* rgba, int32_t w,
                               int32_t h) {
    if (layer == nil || rgba == nullptr || w <= 0 || h <= 0) return;
    const size_t bytes_per_row = static_cast<size_t>(w) * 4u;
    const size_t nbytes = bytes_per_row * static_cast<size_t>(h);
    NSData* copy = [NSData dataWithBytes:rgba length:nbytes];
    CGDataProviderRef provider =
        CGDataProviderCreateWithCFData((__bridge CFDataRef)copy);
    if (!provider) return;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate(
        static_cast<size_t>(w), static_cast<size_t>(h), 8, 32, bytes_per_row, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault, provider,
        nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);
    if (!img) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contents = (__bridge id)img;
    [CATransaction commit];
    CGImageRelease(img);
}

namespace vitreo::windowing {

void CocoaBridge::present_video_overlay(HWND hwnd, const u8* rgba, i32 width,
                                        i32 height, i32 dest_left, i32 dest_top,
                                        i32 dest_width, i32 dest_height) {
    @autoreleasepool {
        if (rgba == nullptr || width <= 0 || height <= 0) return;
        NSWindow* window = nil;
        auto top = m_windows.find(hwnd);
        if (top != m_windows.end() && top->second.ns_window) {
            window = reinterpret_cast<NSWindow*>(top->second.ns_window);
        } else {
            auto ctrl = m_controls.find(hwnd);
            if (ctrl != m_controls.end() && ctrl->second != nullptr) {
                NSView* child = (__bridge NSView*)ctrl->second;
                window = [child window];
            }
        }
        if (window == nil) {
            std::fprintf(stderr,
                         "[cocoa] video overlay skip hwnd=0x%x no NSWindow\n",
                         hwnd);
            return;
        }

        const size_t bytes =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        NSData* pixelData = [NSData dataWithBytes:rgba length:bytes];
        i32 w = width;
        i32 h = height;
        vtr_run_on_main_sync(^{
            NSView* content = [window contentView];
            if (content == nil) {
                std::fprintf(stderr,
                             "[cocoa] video overlay skip hwnd=0x%x no content\n",
                             hwnd);
                return;
            }
            CAMetalLayer* metal = vtr_find_metal_layer(content);
            if (metal == nil) {
                for (const auto& [pres_hwnd, ptr] : m_presentation_views) {
                    (void)pres_hwnd;
                    if (ptr == nullptr) continue;
                    NSView* hosted = (__bridge NSView*)ptr;
                    if (hosted.window != window) continue;
                    metal = vtr_find_metal_layer(hosted);
                    if (metal != nil) break;
                }
            }
            NSRect screen_frame = NSZeroRect;
            if (dest_width > 0 && dest_height > 0) {
                // IVideoWindow Left/Top/Width/Height are owner-client pixels
                // (Y-down). VtrContentView is flipped, so the dest rect maps
                // directly into the content view.
                const NSRect dest = NSMakeRect(
                    static_cast<CGFloat>(dest_left),
                    static_cast<CGFloat>(dest_top),
                    static_cast<CGFloat>(dest_width),
                    static_cast<CGFloat>(dest_height));
                const NSRect window_rect =
                    [content convertRect:dest toView:nil];
                screen_frame = [window convertRectToScreen:window_rect];
            } else {
                screen_frame =
                    vtr_video_overlay_screen_frame(window, content, metal);
            }
            if (screen_frame.size.width < 1.0 ||
                screen_frame.size.height < 1.0) {
                const NSRect bounds =
                    [content convertRect:content.bounds toView:nil];
                screen_frame = [window convertRectToScreen:bounds];
            }
            if (screen_frame.size.width < 1.0 ||
                screen_frame.size.height < 1.0) {
                std::fprintf(stderr,
                             "[cocoa] video overlay skip hwnd=0x%x zero frame\n",
                             hwnd);
                return;
            }

            NSWindow* overlay = nil;
            auto existing = m_video_overlays.find(hwnd);
            if (existing != m_video_overlays.end() &&
                existing->second != nullptr) {
                overlay = (__bridge NSWindow*)existing->second;
            }
            if (overlay == nil) {
                overlay = [[NSWindow alloc]
                    initWithContentRect:screen_frame
                              styleMask:NSWindowStyleMaskBorderless
                                backing:NSBackingStoreBuffered
                                  defer:NO];
                overlay.releasedWhenClosed = NO;
                overlay.opaque = YES;
                overlay.backgroundColor = [NSColor blackColor];
                overlay.hasShadow = NO;
                overlay.ignoresMouseEvents = YES;
                overlay.animationBehavior = NSWindowAnimationBehaviorNone;
                overlay.level = window.level;
                NSView* overlay_content = [overlay contentView];
                [overlay_content setWantsLayer:YES];
                overlay_content.layer.backgroundColor =
                    CGColorGetConstantColor(kCGColorBlack);
                CALayer* layer = overlay_content.layer;
                layer.contentsGravity = kCAGravityResize;
                layer.magnificationFilter = kCAFilterLinear;
                layer.minificationFilter = kCAFilterLinear;
                layer.actions = @{
                    @"contents": [NSNull null],
                    @"frame": [NSNull null],
                    @"bounds": [NSNull null],
                    @"position": [NSNull null]
                };
                vtr_set_layer_rgba(layer, (const uint8_t*)[pixelData bytes],
                                   w, h);
                [window addChildWindow:overlay ordered:NSWindowAbove];
                [overlay orderFront:nil];
                m_video_overlays[hwnd] = (__bridge_retained void*)overlay;
                std::fprintf(stderr,
                             "[cocoa] video overlay window hwnd=0x%x "
                             "%.0fx%.0f+%.0f+%.0f metal=%d\n",
                             hwnd, screen_frame.size.width,
                             screen_frame.size.height, screen_frame.origin.x,
                             screen_frame.origin.y, metal != nil ? 1 : 0);
            } else {
                [overlay setFrame:screen_frame display:YES];
                if ([overlay parentWindow] != window) {
                    NSWindow* old_parent = [overlay parentWindow];
                    if (old_parent != nil) {
                        [old_parent removeChildWindow:overlay];
                    }
                    [window addChildWindow:overlay ordered:NSWindowAbove];
                }
                [overlay orderFront:nil];
            }
            vtr_set_layer_rgba([overlay contentView].layer,
                               (const uint8_t*)[pixelData bytes], w, h);
        });
    }
}

void CocoaBridge::hide_video_overlay(HWND hwnd) {
    vtr_run_on_main_sync(^{
        auto it = m_video_overlays.find(hwnd);
        if (it == m_video_overlays.end() || it->second == nullptr) return;
        vtr_close_video_overlay_window(it->second);
        m_video_overlays.erase(it);
    });
}

} // namespace vitreo::windowing
