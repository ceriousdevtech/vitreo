// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_window_state.mm
/// @brief Window visibility, title, opacity and attention, and the state queries tests use.
///
/// Showing and hiding a top-level window, its title, its opacity and its
/// Dock attention request, and the read-only queries the tests inspect the
/// native window through.
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

void CocoaBridge::set_visible(HWND hwnd, bool visible) {
    vtr_run_on_main_sync(^{
        // Check top-level windows first
        auto it = m_windows.find(hwnd);
        if (it != m_windows.end()) {
            it->second.visible = visible;

            if (diag_logs_enabled()) {
                fprintf(stderr, "[winshow] set_visible(top-level) hwnd=0x%x visible=%d ns_window=%p\n",
                        hwnd, visible ? 1 : 0, it->second.ns_window);
            }
            const HWND owner = it->second.owner;
            if (it->second.is_dialog && owner != NULL_HWND &&
                should_embed_owned_dialog(hwnd, it->second.guest_style, owner)) {
                adopt_extra_family_dialog(hwnd);
            } else if (it->second.ns_window) {
                sync_native_window_visibility(it->second);
                if (visible && std::getenv("VITREO_LOG_PRESENT_VIEW")) {
                    NSWindow* window = reinterpret_cast<NSWindow*>(it->second.ns_window);
                    for (const auto& [surface_hwnd, surface_view] : m_presentation_views) {
                        NSView* surface = (__bridge NSView*)surface_view;
                        if ([surface window] == window) {
                            debug_describe_presentation_view(surface_hwnd, "after-show");
                        }
                    }
                }
                return;
            } else {
                return;
            }
        }

        // Check child controls
        auto ctrl_it = m_controls.find(hwnd);
        if (ctrl_it != m_controls.end()) {
            NSView* view = (__bridge NSView*)ctrl_it->second;
            if (visible && [view isKindOfClass:[VtrContainerView class]]) {
                NSView* parent = [view superview];
                if (parent) {
                    [parent addSubview:view positioned:NSWindowAbove relativeTo:nil];
                }
            }
            [view setHidden:!visible];
            // Remember an explicit guest hide so a later show of the host
            // window does not bring the control back (see
            // sync_native_window_visibility).
            if (visible) {
                m_controls_hidden_by_guest.erase(hwnd);
            } else {
                m_controls_hidden_by_guest.insert(hwnd);
            }
            // A child is on screen only while its top-level is (Learn
            // IsWindowVisible: visible only if the window and all its parents
            // are), and showing a child activates nothing (SWP_NOACTIVATE for
            // WS_CHILD, measured setparent_child_to_desktop). So an off-screen
            // host follows ITS OWN top-level's Win32 visibility, through the
            // ordinary sync and its activation rules -- never a bare
            // makeKeyAndOrderFront. That put a still-hidden
            // top-level on screen and made it key when its compositor child
            // was shown. USER32 then activated the hidden window before the
            // guest had built its widget, a NULL dereference in WM_SETFOCUS.
            if (visible) {
                NSWindow* host = [view window];
                if (host != nil && ![host isVisible]) {
                    const HWND top = find_hwnd_for_window((__bridge void*)host);
                    auto top_it = m_windows.find(top);
                    if (top_it != m_windows.end() && top_it->second.visible &&
                        top_it->second.ns_window != nullptr) {
                        sync_native_window_visibility(top_it->second);
                    }
                }
            }

            VITREO_TRACE( "[cocoa] set_visible: child control 0x%x visible=%s\n",
                         hwnd, visible ? "yes" : "no");
        }
    });
}

void CocoaBridge::set_title(HWND hwnd, std::wstring_view title) {
    if (![NSThread isMainThread]) {
        std::wstring title_copy(title);
        vtr_run_on_main_async(^{
            this->set_title(hwnd, title_copy);
        });
        return;
    }

    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) return;

    it->second.title = std::wstring(title);

    // Same main-thread requirement as set_frame().
    vtr_run_on_main_sync(^{
        if (it->second.ns_window) {
            auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
            [win setTitle:to_nsstring(it->second.title)];
        }
    });
}

// =============================================================================
// Test Helpers
// =============================================================================

bool CocoaBridge::has_native_window(HWND hwnd) const {
    return m_windows.find(hwnd) != m_windows.end();
}

bool CocoaBridge::native_window_is_visible(HWND hwnd) const {
    __block bool visible = false;
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || it->second.ns_window == nullptr) return;
        NSWindow* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        visible = [win isVisible];
    });
    return visible;
}

bool CocoaBridge::native_window_is_resizable(HWND hwnd) const {
    __block bool resizable = false;
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || !it->second.ns_window) return;
        NSWindow* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        resizable = ([win styleMask] & NSWindowStyleMaskResizable) != 0;
    });
    return resizable;
}

bool CocoaBridge::native_window_is_opaque(HWND hwnd) const {
    __block bool opaque = true;
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || it->second.ns_window == nullptr) return;
        NSWindow* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        opaque = [win isOpaque];
    });
    return opaque;
}

bool CocoaBridge::native_window_ignores_mouse_events(HWND hwnd) const {
    __block bool ignores = false;
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || it->second.ns_window == nullptr) return;
        NSWindow* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        ignores = [win ignoresMouseEvents];
    });
    return ignores;
}

int CocoaBridge::native_pixel_contents_alpha_info(HWND hwnd) const {
    __block int info = -1;
    vtr_run_on_main_sync(^{
        VtrPixelView* pixel = nil;
        auto pit = m_guest_pixel_views.find(hwnd);
        if (pit != m_guest_pixel_views.end() && pit->second != nullptr) {
            pixel = (__bridge VtrPixelView*)pit->second;
        }
        if (pixel == nil) {
            auto it = m_windows.find(hwnd);
            if (it == m_windows.end() || it->second.ns_window == nullptr) {
                return;
            }
            NSWindow* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
            NSView* content = [win contentView];
            if ([content isKindOfClass:[VtrPixelView class]]) {
                pixel = (VtrPixelView*)content;
            } else {
                for (NSView* sub in [content subviews]) {
                    if ([sub isKindOfClass:[VtrPixelView class]]) {
                        pixel = (VtrPixelView*)sub;
                        break;
                    }
                }
            }
        }
        if (pixel == nil || pixel.layer.contents == nil) {
            return;
        }
        CGImageRef img = (__bridge CGImageRef)pixel.layer.contents;
        info = static_cast<int>(CGImageGetAlphaInfo(img));
    });
    return info;
}

bool CocoaBridge::native_content_hits_point(HWND hwnd, i32 x, i32 y) const {
    __block bool hits = false;
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || it->second.ns_window == nullptr) return;
        NSWindow* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        NSView* content = [win contentView];
        if (content == nil) return;
        const NSPoint local = NSMakePoint(static_cast<CGFloat>(x), static_cast<CGFloat>(y));
        hits = [content hitTest:[content convertPoint:local toView:content.superview]] != nil;
    });
    return hits;
}

usize CocoaBridge::native_window_count() const {
    return m_windows.size();
}

void CocoaBridge::show_without_activation(HWND hwnd) {
    // Minimal presentation hook for the USER32 activation contract: the
    // first native show normally makes an activating window key, which a
    // SW_SHOWNOACTIVATE / SWP_NOACTIVATE show must not do.
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it != m_windows.end()) it->second.show_without_activation = true;
    });
    set_visible(hwnd, true);
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it != m_windows.end()) it->second.show_without_activation = false;
    });
}

bool CocoaBridge::set_window_opacity(HWND hwnd, u8 alpha) {
    // SetLayeredWindowAttributes(LWA_ALPHA): the whole top-level window,
    // frame included, at a constant opacity. Child windows have no NSWindow.
    __block bool applied = false;
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || it->second.ns_window == nullptr) return;
        NSWindow* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        [win setAlphaValue:static_cast<CGFloat>(alpha) / 255.0];
        applied = true;
    });
    return applied;
}

bool CocoaBridge::set_window_per_pixel_alpha(HWND hwnd, bool enabled) {
    __block bool applied = false;
    vtr_run_on_main_sync(^{
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || it->second.ns_window == nullptr) return;
        NativeWindowState& state = it->second;
        NSWindow* win = reinterpret_cast<NSWindow*>(state.ns_window);
        applied = true;
        if (enabled == state.layered_alpha) return;
        state.layered_alpha = enabled;
        if (enabled) {
            state.pre_layered_opaque = [win isOpaque];
            state.pre_layered_shadow = [win hasShadow];
            state.pre_layered_background =
                (void*)CFBridgingRetain([win backgroundColor]);
            apply_layered_alpha_appearance(win);
        } else {
            [win setOpaque:state.pre_layered_opaque ? YES : NO];
            if (state.pre_layered_background != nullptr) {
                NSColor* color =
                    (NSColor*)CFBridgingRelease(state.pre_layered_background);
                state.pre_layered_background = nullptr;
                [win setBackgroundColor:color];
                NSView* content = [win contentView];
                if (content != nil && content.wantsLayer) {
                    content.layer.backgroundColor = color.CGColor;
                }
            }
            [win setHasShadow:state.pre_layered_shadow ? YES : NO];
        }
        auto pixel_it = m_guest_pixel_views.find(hwnd);
        if (pixel_it != m_guest_pixel_views.end() && pixel_it->second != nullptr) {
            ((__bridge VtrPixelView*)pixel_it->second).compositesAlpha =
                enabled ? YES : NO;
        }
    });
    return applied;
}

void CocoaBridge::request_user_attention(HWND hwnd, bool start) {
    (void)hwnd;  // Dock attention is per application, not per window.
    static std::atomic<long> s_request{0};
    vtr_run_on_main_async(^{
        if (NSApp == nil) return;
        const long previous = s_request.exchange(0);
        if (previous != 0) [NSApp cancelUserAttentionRequest:previous];
        if (start) {
            s_request.store(static_cast<long>(
                [NSApp requestUserAttention:NSInformationalRequest]));
        }
    });
}

HWND CocoaBridge::find_hwnd_for_window(void* ns_win) const {
    for (const auto& [hwnd, state] : m_windows) {
        if (state.ns_window && reinterpret_cast<NSWindow*>(state.ns_window) == reinterpret_cast<NSWindow*>(ns_win)) {
            return hwnd;
        }
    }
    return NULL_HWND;
}

} // namespace vitreo::windowing
