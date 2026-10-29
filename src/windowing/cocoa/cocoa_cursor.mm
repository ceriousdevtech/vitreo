// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_cursor.mm
/// @brief Cursor shape and visibility.
///
/// SetCursor, ShowCursor and the system cursor IDs, applied through AppKit's
/// cursor rectangles: a hidden guest cursor, a custom or animated cursor
/// built from guest pixels, and the system shapes.
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

// Guest SetCursor(NULL) / ShowCursor(FALSE) hide the OS pointer so the game
// can draw its own. AppKit still sends cursorUpdate:/resetCursorRects to
// every view under the mouse. [super cursorUpdate:] installs the macOS
// arrow. A 1×1 transparent NSCursor is rejected and also becomes the arrow
// — that is the arrow that appears on the first click while the guest
// hides the pointer. When hidden, do
// not replace the shape and do not call super.
static vitreo::windowing::CocoaBridge* vtr_bridge_from_view(NSView* view) {
    id del = view.window.delegate;
    if ([del isKindOfClass:[VtrWindowDelegate class]]) {
        return ((VtrWindowDelegate*)del).bridge;
    }
    return nullptr;
}

// AppKit cursor rectangles scope a hidden guest cursor to the guest view.
// A normal-sized transparent image is intentional: macOS rejects a 1x1
// transparent cursor and substitutes the arrow.
static NSCursor* vtr_hidden_window_cursor() {
    static NSCursor* cursor = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        constexpr NSInteger kCursorSize = 16;
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:nullptr
                          pixelsWide:kCursorSize
                          pixelsHigh:kCursorSize
                       bitsPerSample:8
                     samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                      colorSpaceName:NSDeviceRGBColorSpace
                         bytesPerRow:kCursorSize * 4
                        bitsPerPixel:32];
        std::memset([rep bitmapData], 0,
                    static_cast<size_t>(kCursorSize * kCursorSize * 4));
        NSImage* image = [[NSImage alloc]
            initWithSize:NSMakeSize(kCursorSize, kCursorSize)];
        [image addRepresentation:rep];
        cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
        [image release];
        [rep release];
    });
    return cursor;
}

// YES: applied a hidden or installed cursor. NO: caller may invoke super.
BOOL vtr_view_reset_cursor_rects(NSView* view) {
    auto* bridge = vtr_bridge_from_view(view);
    if (bridge != nullptr && bridge->is_cursor_hidden()) {
        [view addCursorRect:view.bounds cursor:vtr_hidden_window_cursor()];
        return YES;
    }
    NSCursor* cursor = (bridge != nullptr)
        ? (__bridge NSCursor*)bridge->installed_ns_cursor()
        : nil;
    if (cursor) {
        [view addCursorRect:view.bounds cursor:cursor];
        return YES;
    }
    return NO;
}

BOOL vtr_view_cursor_update(NSView* view) {
    auto* bridge = vtr_bridge_from_view(view);
    if (bridge != nullptr && bridge->is_cursor_hidden()) {
        [vtr_hidden_window_cursor() set];
        return YES;
    }
    NSCursor* cursor = (bridge != nullptr)
        ? (__bridge NSCursor*)bridge->installed_ns_cursor()
        : nil;
    if (cursor) {
        [cursor set];
        return YES;
    }
    return NO;
}

namespace vitreo::windowing {

void CocoaBridge::apply_host_cursor_visibility() {
    vtr_run_on_main_sync(^{
        bool exclusive = false;
        for (const auto& [hwnd, state] : m_windows) {
            (void)hwnd;
            if (state.fullscreen) {
                exclusive = true;
                break;
            }
        }
        const bool want_hidden = host_cursor_should_hide(
            m_cursor_hidden, exclusive, m_custom_ns_cursor != nullptr,
            [NSApp isActive] == YES);
        if (diag::input_diag()) {
            std::fprintf(stderr,
                         "[input] host cursor guest_hidden=%u exclusive=%u "
                         "custom=%u active=%u -> display_hidden=%u "
                         "(was %u)\n",
                         m_cursor_hidden ? 1u : 0u, exclusive ? 1u : 0u,
                         m_custom_ns_cursor != nullptr ? 1u : 0u,
                         [NSApp isActive] == YES ? 1u : 0u,
                         want_hidden ? 1u : 0u,
                         m_host_cursor_hide_applied ? 1u : 0u);
        }
        if (want_hidden && !m_host_cursor_hide_applied) {
            CGDisplayHideCursor(kCGDirectMainDisplay);
            m_host_cursor_hide_applied = true;
        } else if (!want_hidden && m_host_cursor_hide_applied) {
            CGDisplayShowCursor(kCGDirectMainDisplay);
            m_host_cursor_hide_applied = false;
        }
        if (!want_hidden) {
            if (m_cursor_hidden) {
                [vtr_hidden_window_cursor() set];
            } else if (m_custom_ns_cursor) {
                [(__bridge NSCursor*)m_custom_ns_cursor set];
            }
        }
    });
}

void CocoaBridge::set_cursor_visible(bool visible) {
    // This is a synchronous hop onto the Cocoa main thread, and that thread is
    // concurrently servicing window-server events -- so a caller that asks for
    // a state the cursor is already in still blocks for a full round trip.
    // The guest does exactly that once per frame: SetCursor(NULL) on
    // its software-cursor path. Skip the
    // hop when neither the requested visibility nor the installed shape has
    // moved since the last call we actually applied.
    const int want = visible ? 1 : 0;
    const unsigned shape_gen = m_cursor_shape_gen.load(std::memory_order_acquire);
    if (m_cursor_visible_req.load(std::memory_order_acquire) == want &&
        m_cursor_shape_applied.load(std::memory_order_acquire) == shape_gen) {
        return;
    }
    m_cursor_visible_req.store(want, std::memory_order_release);
    vtr_run_on_main_sync(^{
        // Stamped inside the block so a shape swap that lands between the
        // load above and the hop re-arms the next caller instead of being
        // swallowed by the cache.
        m_cursor_shape_applied.store(shape_gen, std::memory_order_release);
        // Always use CGDisplayHideCursor / ShowCursor. [NSCursor hide] is
        // undone by AppKit cursorUpdate: on the next mouse move. Mixing the
        // two APIs leaves the hide count permanently off.
        //
        // disableCursorRects is the wrong tool here: AppKit then ignores our
        // invisible cursor rects and restores the default macOS arrow — that
        // is the arrow the guest kept showing after SetCursor(NULL).
        auto apply_shape = ^{
            if (m_custom_ns_cursor) {
                [(__bridge NSCursor*)m_custom_ns_cursor set];
            } else {
                [[NSCursor arrowCursor] set];
            }
        };
        auto sync_window_rects = ^{
            for (auto& [hwnd, state] : m_windows) {
                (void)hwnd;
                if (!state.ns_window) continue;
                NSWindow* win = reinterpret_cast<NSWindow*>(state.ns_window);
                if (![win areCursorRectsEnabled]) {
                    [win enableCursorRects];
                }
                vtr_invalidate_cursor_rects_recursive(win, [win contentView]);
            }
        };
        const bool was_hidden = m_cursor_hidden;
        m_cursor_hidden = !visible;
        apply_host_cursor_visibility();
        if (visible && was_hidden) {
            apply_shape();
        }
        if (was_hidden != m_cursor_hidden) {
            sync_window_rects();
        }
    });
}

/// Build an NSCursor from tightly packed top-down RGBA8. Main thread.
static NSCursor* vtr_cursor_from_rgba(vitreo::u32 width, vitreo::u32 height,
                                      vitreo::u32 hotspot_x,
                                      vitreo::u32 hotspot_y,
                                      const vitreo::u8* pixels,
                                      vitreo::usize byte_count) {
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL
                      pixelsWide:static_cast<NSInteger>(width)
                      pixelsHigh:static_cast<NSInteger>(height)
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                    bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                     bytesPerRow:static_cast<NSInteger>(width) * 4
                    bitsPerPixel:32];
    if (!rep || ![rep bitmapData]) return nil;
    std::memcpy([rep bitmapData], pixels, byte_count);
    if (const char* directory = std::getenv("VITREO_DUMP_CURSOR_DIR")) {
        static unsigned sequence = 0;
        if (sequence < 64) {
            unsigned opaque = 0, transparent = 0;
            for (vitreo::usize i = 3; i < byte_count; i += 4) {
                opaque += pixels[i] == 255;
                transparent += pixels[i] == 0;
            }
            NSString* path = [NSString stringWithFormat:@"%s/cursor-%u.png", directory, sequence++];
            NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            [png writeToFile:path atomically:YES];
            std::fprintf(stderr, "[cursor-image] %ux%u hotspot=%u,%u opaque=%u transparent=%u file=%s\n",
                         width, height, hotspot_x, hotspot_y, opaque, transparent, path.UTF8String);
        }
    }

    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    if (!image) return nil;
    [image addRepresentation:rep];
    return [[NSCursor alloc] initWithImage:image
                                   hotSpot:NSMakePoint(hotspot_x, hotspot_y)];
}

void CocoaBridge::stop_cursor_animation() {
    if (m_cursor_anim_timer) {
        [(__bridge NSTimer*)m_cursor_anim_timer invalidate];
        CFRelease((CFTypeRef)m_cursor_anim_timer);
        m_cursor_anim_timer = nullptr;
    }
    for (void* frame : m_cursor_anim_frames) {
        if (frame) CFRelease((CFTypeRef)frame);
    }
    m_cursor_anim_frames.clear();
    m_cursor_anim_delays.clear();
    m_cursor_anim_step = 0;
}

void CocoaBridge::advance_cursor_animation() {
    if (m_cursor_anim_frames.size() < 2) return;
    m_cursor_anim_step = (m_cursor_anim_step + 1) % m_cursor_anim_frames.size();
    NSCursor* frame =
        (__bridge NSCursor*)m_cursor_anim_frames[m_cursor_anim_step];
    if (m_custom_ns_cursor) {
        CFRelease((CFTypeRef)m_custom_ns_cursor);
    }
    m_custom_ns_cursor = (void*)CFBridgingRetain(frame);
    m_cursor_shape_gen.fetch_add(1, std::memory_order_release);
    if (!m_cursor_hidden) {
        [frame set];
        for (auto& [hwnd, state] : m_windows) {
            (void)hwnd;
            if (!state.ns_window) continue;
            NSWindow* win = reinterpret_cast<NSWindow*>(state.ns_window);
            vtr_invalidate_cursor_rects_recursive(win, [win contentView]);
        }
    }
    schedule_cursor_animation_tick();
}

void CocoaBridge::schedule_cursor_animation_tick() {
    if (m_cursor_anim_timer) {
        [(__bridge NSTimer*)m_cursor_anim_timer invalidate];
        CFRelease((CFTypeRef)m_cursor_anim_timer);
        m_cursor_anim_timer = nullptr;
    }
    if (m_cursor_anim_frames.size() < 2) return;
    const vitreo::u32 delay_ms =
        m_cursor_anim_step < m_cursor_anim_delays.size()
            ? m_cursor_anim_delays[m_cursor_anim_step]
            : 100u;
    // One-shot and re-armed each tick: ANI rates are per frame, so a single
    // repeating timer would have to run at the shortest one and skip.
    NSTimer* timer = [NSTimer
        timerWithTimeInterval:static_cast<double>(delay_ms) / 1000.0
                      repeats:NO
                        block:^(NSTimer* t) {
                            (void)t;
                            advance_cursor_animation();
                        }];
    // Common modes: the guest pump runs in the default mode, but AppKit
    // switches to a tracking mode while a mouse button is held, and a busy
    // cursor that freezes on click is exactly the bug this animation is for.
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    m_cursor_anim_timer = (void*)CFBridgingRetain(timer);
}

bool CocoaBridge::set_animated_cursor_rgba(
    u32 width, u32 height, u32 hotspot_x, u32 hotspot_y,
    const std::vector<std::vector<u8>>& frames,
    const std::vector<u32>& delays_ms) {
    const usize expected =
        static_cast<usize>(width) * static_cast<usize>(height) * 4u;
    if (width == 0 || height == 0 || frames.size() < 2 ||
        frames.size() != delays_ms.size()) {
        return false;
    }
    for (const auto& frame : frames) {
        if (frame.size() < expected) return false;
    }
    std::vector<std::vector<u8>> pixels = frames;
    std::vector<u32> delays = delays_ms;
    __block bool ok = false;
    vtr_run_on_main_sync(^{
        std::vector<void*> built;
        built.reserve(pixels.size());
        for (const auto& frame : pixels) {
            NSCursor* cursor = vtr_cursor_from_rgba(
                width, height, hotspot_x, hotspot_y, frame.data(), expected);
            if (!cursor) break;
            built.push_back((void*)CFBridgingRetain(cursor));
        }
        if (built.size() != pixels.size()) {
            for (void* f : built) CFRelease((CFTypeRef)f);
            return;
        }
        stop_cursor_animation();
        m_cursor_anim_frames = std::move(built);
        m_cursor_anim_delays = std::move(delays);
        m_cursor_anim_step = 0;
        if (m_custom_ns_cursor) {
            CFRelease((CFTypeRef)m_custom_ns_cursor);
        }
        m_custom_ns_cursor =
            (void*)CFBridgingRetain((__bridge NSCursor*)m_cursor_anim_frames[0]);
        m_cursor_shape_gen.fetch_add(1, std::memory_order_release);
        apply_host_cursor_visibility();
        for (auto& [hwnd, state] : m_windows) {
            (void)hwnd;
            if (!state.ns_window) continue;
            NSWindow* win = reinterpret_cast<NSWindow*>(state.ns_window);
            vtr_invalidate_cursor_rects_recursive(win, [win contentView]);
        }
        schedule_cursor_animation_tick();
        ok = true;
    });
    return ok;
}

bool CocoaBridge::set_custom_cursor_rgba(u32 width, u32 height,
                                         u32 hotspot_x, u32 hotspot_y,
                                         const u8* rgba, usize byte_count) {
    const usize expected =
        static_cast<usize>(width) * static_cast<usize>(height) * 4u;
    // Hotspot may lie outside the image (Learn does not reject that); AppKit
    // still accepts the NSPoint.
    if (rgba == nullptr || width == 0 || height == 0 ||
        byte_count < expected) {
        return false;
    }

    // Copy before hopping to the main thread — the caller may free @p rgba.
    std::vector<u8> pixels(rgba, rgba + expected);
    __block bool ok = false;
    vtr_run_on_main_sync(^{
        NSCursor* cursor = vtr_cursor_from_rgba(width, height, hotspot_x,
                                                hotspot_y, pixels.data(),
                                                expected);
        if (!cursor) {
            return;
        }
        stop_cursor_animation();
        if (m_custom_ns_cursor) {
            CFRelease((CFTypeRef)m_custom_ns_cursor);
            m_custom_ns_cursor = nullptr;
        }
        m_custom_ns_cursor = (void*)CFBridgingRetain(cursor);
        m_cursor_shape_gen.fetch_add(1, std::memory_order_release);
        apply_host_cursor_visibility();
        for (auto& [hwnd, state] : m_windows) {
            (void)hwnd;
            if (!state.ns_window) continue;
            NSWindow* win = reinterpret_cast<NSWindow*>(state.ns_window);
            vtr_invalidate_cursor_rects_recursive(win, [win contentView]);
        }
        ok = true;
    });
    return ok;
}

void CocoaBridge::set_system_cursor(u32 idc) {
    vtr_run_on_main_sync(^{
        NSCursor* cursor = [NSCursor arrowCursor];
        switch (idc) {
            case 32513: // IDC_IBEAM
                cursor = [NSCursor IBeamCursor];
                break;
            case 32515: // IDC_CROSS
                cursor = [NSCursor crosshairCursor];
                break;
            case 32644: // IDC_SIZEWE
                cursor = [NSCursor resizeLeftRightCursor];
                break;
            case 32645: // IDC_SIZENS
                cursor = [NSCursor resizeUpDownCursor];
                break;
            case 32646: // IDC_SIZEALL
                cursor = [NSCursor openHandCursor];
                break;
            case 32648: // IDC_NO
                cursor = [NSCursor operationNotAllowedCursor];
                break;
            case 32649: // IDC_HAND
                cursor = [NSCursor pointingHandCursor];
                break;
            case 32514: // IDC_WAIT
            case 32650: // IDC_APPSTARTING
                // Windows' busy shapes have no public NSCursor. macOS carries
                // one behind a private selector, and it is the mapping every
                // browser engine uses for CSS cursor:wait / cursor:progress.
                // Guarded, because falling back to the arrow is exactly the
                // behaviour we are fixing and must not become a crash.
                if ([NSCursor respondsToSelector:@selector(busyButClickableCursor)]) {
                    NSCursor* busy = [NSCursor performSelector:
                                          @selector(busyButClickableCursor)];
                    if (busy != nil) cursor = busy;
                }
                break;
            default:
                break;
        }
        stop_cursor_animation();
        if (m_custom_ns_cursor) {
            CFRelease((CFTypeRef)m_custom_ns_cursor);
            m_custom_ns_cursor = nullptr;
        }
        m_custom_ns_cursor = (void*)CFBridgingRetain(cursor);
        m_cursor_shape_gen.fetch_add(1, std::memory_order_release);
        apply_host_cursor_visibility();
        for (auto& [hwnd, state] : m_windows) {
            (void)hwnd;
            if (!state.ns_window) continue;
            NSWindow* win = reinterpret_cast<NSWindow*>(state.ns_window);
            vtr_invalidate_cursor_rects_recursive(win, [win contentView]);
        }
    });
}

void CocoaBridge::reassert_cursor_for_mouse_event(void* ns_event) {
    // AppKit disables cursor RECTANGLES while a mouse button is down, and the
    // custom shape is only ever established through that machinery
    // (resetCursorRects / cursorUpdate:). So a click-and-hold drops the guest's
    // cursor back to the window default -- the macOS arrow -- until release.
    // A direct [cursor set] is outside the rect machinery and survives the
    // drag. Main thread only: the event pump already runs there, so this must
    // not take the vtr_run_on_main_sync hop on every drag event.
    NSEvent* event = (__bridge NSEvent*)ns_event;
    switch ([event type]) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseDragged:
        break;
    default:
        return;
    }
    if (diag::input_diag()) {
        NSCursor* installed = m_custom_ns_cursor != nullptr
            ? (__bridge NSCursor*)m_custom_ns_cursor
            : nil;
        std::fprintf(stderr,
                     "[input] mouse-down cursor reassert hidden=%d "
                     "installed=%d arrow=%d\n",
                     m_cursor_hidden ? 1 : 0,
                     installed != nil ? 1 : 0,
                     installed == [NSCursor arrowCursor] ? 1 : 0);
    }
    if (m_cursor_hidden || m_custom_ns_cursor == nullptr) {
        return;
    }
    [(__bridge NSCursor*)m_custom_ns_cursor set];
}

void CocoaBridge::reapply_installed_cursor() {
    vtr_run_on_main_sync(^{
        apply_host_cursor_visibility();
        if (m_cursor_hidden || m_custom_ns_cursor == nullptr) {
            return;
        }
        [(__bridge NSCursor*)m_custom_ns_cursor set];
        for (auto& [hwnd, state] : m_windows) {
            (void)hwnd;
            if (!state.ns_window) continue;
            NSWindow* win = reinterpret_cast<NSWindow*>(state.ns_window);
            vtr_invalidate_cursor_rects_recursive(win, [win contentView]);
        }
    });
}

} // namespace vitreo::windowing
