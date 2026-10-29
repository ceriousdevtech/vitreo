// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_display.mm
/// @brief Display topology: monitors, the virtual desktop, and window visibility against it.
///
/// Win32 monitor geometry is a virtual desktop of device-pixel rectangles with the primary at the origin. NSScreen describes the same displays in points with a bottom-left origin, so every value here is a translation, not a passthrough.
///
/// Split out of cocoa_bridge.mm, which was 8,675 lines. These are CocoaBridge
/// methods defined in a separate translation unit; the class is unchanged.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <atomic>
#include <mutex>
#include "windowing/cocoa/window_presentation.hpp"
#include "windowing/cocoa/app_icon_presentation.hpp"
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <algorithm>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreGraphics/CGEventSource.h>
#include <mach/mach_time.h>

namespace vitreo::windowing {

// =============================================================================
// Native Delegation: Monitor Info (NSScreen)
// =============================================================================

namespace {

/// Cocoa global coords are Y-up; Win32 screen coords are Y-down with the
/// virtual desktop top at y=0. Convert one NSScreen frame into Win32 space
/// given the global Cocoa max Y (top of the virtual desktop in Cocoa).
CocoaBridge::MonitorInfo monitor_from_nsscreen(NSScreen* screen,
                                               CGFloat cocoa_top_y,
                                               bool primary) {
    CocoaBridge::MonitorInfo info;
    NSRect frame = [screen frame];
    NSRect visible = [screen visibleFrame];
    info.scale_factor = static_cast<double>([screen backingScaleFactor]);
    info.primary = primary;
    NSNumber* bits_per_sample = [[screen deviceDescription]
        objectForKey:NSDeviceBitsPerSample];
    if (bits_per_sample && [bits_per_sample unsignedIntValue] != 0) {
        info.bits_per_color = [bits_per_sample unsignedIntValue];
    }
    NSNumber* screen_number = [[screen deviceDescription]
        objectForKey:@"NSScreenNumber"];
    if (screen_number) {
        const CGDirectDisplayID display_id =
            static_cast<CGDirectDisplayID>([screen_number unsignedIntValue]);
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display_id);
        if (mode) {
            info.refresh_rate_hz = CGDisplayModeGetRefreshRate(mode);
            CGDisplayModeRelease(mode);
        }
        // Physical panel size for GetDpiForMonitor(MDT_RAW_DPI).
        const CGSize mm = CGDisplayScreenSize(display_id);
        info.physical_width_mm = mm.width;
        info.physical_height_mm = mm.height;
    }
    // Variable-refresh displays can report 0 for the current CoreGraphics
    // mode. NSScreen's maximum frame rate is then the truthful host mode
    // capability available to user space.
    if (info.refresh_rate_hz <= 0.0) {
        info.refresh_rate_hz =
            static_cast<double>([screen maximumFramesPerSecond]);
    }

    info.left   = static_cast<i32>(frame.origin.x);
    info.top    = static_cast<i32>(cocoa_top_y - (frame.origin.y + frame.size.height));
    info.right  = static_cast<i32>(frame.origin.x + frame.size.width);
    info.bottom = static_cast<i32>(cocoa_top_y - frame.origin.y);
    info.width  = info.right - info.left;
    info.height = info.bottom - info.top;

    info.work_left   = static_cast<i32>(visible.origin.x);
    info.work_top    = static_cast<i32>(
        cocoa_top_y - (visible.origin.y + visible.size.height));
    info.work_right  = static_cast<i32>(visible.origin.x + visible.size.width);
    info.work_bottom = static_cast<i32>(cocoa_top_y - visible.origin.y);
    return info;
}


} // namespace

CGFloat cocoa_virtual_top_y() {
    // Win32 screen coordinates start at the PRIMARY monitor's top-left (a
    // monitor above it has a negative top), and CoreGraphics global
    // coordinates flip against the same screen: [NSScreen screens][0], the
    // menu-bar screen at the Cocoa origin. Window placement uses the same
    // height (vtr_win32_flip_height in cocoa_bridge_internal.h); flipping
    // monitors against the tallest screen instead put them on a different row
    // than the windows on them whenever a display extends above the primary.
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if ([screens count] == 0) return 0;
    return NSMaxY([[screens objectAtIndex:0] frame]);
}

bool CocoaBridge::guest_rect_intersects_any_monitor(const Rect& r) const {
    if (r.right <= r.left || r.bottom <= r.top) {
        return false;
    }
    const auto monitors = get_all_monitor_info();
    if (monitors.empty()) {
        // No NSScreen yet (headless / early init) — allow show.
        return true;
    }
    for (const auto& mi : monitors) {
        if (r.left < mi.right && r.right > mi.left &&
            r.top < mi.bottom && r.bottom > mi.top) {
            return true;
        }
    }
    return false;
}

bool CocoaBridge::composition_host_defers_to_owned(
    const NativeWindowState& state) const {
    if (state.fullscreen) {
        return false;
    }
    bool owns_other = false;
    for (const auto& [hwnd, other] : m_windows) {
        if (hwnd != state.hwnd && other.owner == state.hwnd) {
            owns_other = true;
            break;
        }
    }
    const auto kind = classify_window_presentation(make_presentation_signals(
        state.guest_style, state.guest_ex_style, state.is_dialog,
        state.client_drawn_frame, state.owner != NULL_HWND, owns_other,
        state.fullscreen));
    return kind == WindowPresentationKind::CompositionHost;
}

HWND CocoaBridge::family_presentation_hwnd(HWND owner) const {
    if (owner == NULL_HWND) {
        return NULL_HWND;
    }
    HWND best = NULL_HWND;
    const auto consider = [&](HWND hwnd, const NativeWindowState& s) {
        if (hwnd == NULL_HWND || !s.ns_window || s.empty_window_rgn) {
            return;
        }
        const bool captioned = (s.guest_style & WS::CAPTION) == WS::CAPTION;
        if (!s.is_dialog && !captioned) {
            return;
        }
        const bool borderless_popup =
            (s.guest_style & WS::POPUP) != 0 &&
            (s.guest_style & WS::CHILD) == 0 &&
            !captioned;
        if (borderless_popup && !s.fullscreen) {
            return;
        }
        if (best == NULL_HWND || (captioned && s.is_dialog)) {
            best = hwnd;
        }
    };
    auto oit = m_windows.find(owner);
    if (oit != m_windows.end()) {
        consider(owner, oit->second);
    }
    for (const auto& [hwnd, s] : m_windows) {
        if (s.owner == owner) {
            consider(hwnd, s);
        }
    }
    return best;
}

bool CocoaBridge::control_is_hidden_by_guest(void* ns_view) const {
    if (ns_view == nullptr || m_controls_hidden_by_guest.empty()) {
        return false;
    }
    for (HWND hwnd : m_controls_hidden_by_guest) {
        auto it = m_controls.find(hwnd);
        if (it != m_controls.end() && it->second == ns_view) {
            return true;
        }
    }
    return false;
}

bool CocoaBridge::should_embed_owned_dialog(HWND dialog, u32 dialog_style,
                                            HWND owner) const {
    // Rule 1 and the "Modal Win32 dialogs" row of
    // .docs/windowing-native-presentation.md: a dialog carrying a standard
    // caption is a top-level window in its own right and gets native chrome —
    // separate NSWindow by default. View-hosting exists for the *chromeless*
    // members of a composition-host family: successive wizard pages that the
    // host paints in place. Embedding a captioned modal dialog strips its
    // title bar and leaves its controls inside an NSWindow that DialogBox has
    // just disabled for the owner, so the dialog cannot be clicked either.
    if ((dialog_style & WS::CAPTION) == WS::CAPTION) {
        return false;
    }
    const HWND host = family_presentation_hwnd(owner);
    if (host == NULL_HWND || host == dialog) {
        return false;
    }
    if (host != owner) {
        return true;
    }
    auto oit = m_windows.find(owner);
    if (oit == m_windows.end()) {
        return false;
    }
    // Extra wizard pages embed into an existing family dialog. A stock
    // DialogBox owned by a captioned document/game HWND must not.
    if (oit->second.is_dialog) {
        return true;
    }
    return (oit->second.guest_style & WS::CAPTION) != WS::CAPTION;
}

void CocoaBridge::adopt_extra_family_dialog(HWND hwnd) {
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end() || !it->second.is_dialog ||
        it->second.owner == NULL_HWND) {
        return;
    }
    if (!should_embed_owned_dialog(hwnd, it->second.guest_style,
                                   it->second.owner)) {
        return;
    }
    const HWND host = family_presentation_hwnd(it->second.owner);
    if (host == NULL_HWND || host == hwnd) {
        return;
    }
    if (m_controls.find(hwnd) == m_controls.end()) {
        WindowData data{};
        const i32 w = it->second.frame.right - it->second.frame.left;
        const i32 h = it->second.frame.bottom - it->second.frame.top;
        data.rect = {0, 0, w > 0 ? w : 1, h > 0 ? h : 1};
        data.visible = it->second.visible;
        add_container_view(host, hwnd, data);
    }
    if (it->second.ns_window) {
        auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
        [win setDelegate:nil];
        [win orderOut:nil];
        [win close];
        it->second.ns_window = nullptr;
        it->second.ns_delegate = nullptr;
    }
    m_windows.erase(it);
}

void CocoaBridge::adopt_extra_dialogs_in_family(HWND root) {
    if (root == NULL_HWND) {
        return;
    }
    const HWND host = family_presentation_hwnd(root);
    if (host == NULL_HWND) {
        return;
    }
    std::vector<HWND> extras;
    for (const auto& [hwnd, state] : m_windows) {
        if (hwnd != host && state.is_dialog && state.owner == root &&
            state.ns_window) {
            extras.push_back(hwnd);
        }
    }
    for (HWND hwnd : extras) {
        adopt_extra_family_dialog(hwnd);
    }
}

void CocoaBridge::sync_native_window_visibility(NativeWindowState& state) {
    if (!state.ns_window) {
        return;
    }
    auto* win = reinterpret_cast<NSWindow*>(state.ns_window);
    // VITREO_LOG_PRESENT_VIEW: record every real on/off-screen transition of a
    // window, with what its presentation views hold at that moment.
    struct VisibilityTrace {
        CocoaBridge& bridge;
        NativeWindowState& state;
        NSWindow* win;
        bool was;
        ~VisibilityTrace() {
            static const bool on = std::getenv("VITREO_LOG_PRESENT_VIEW") != nullptr;
            if (!on) return;
            const bool now = [win isVisible];
            if (now == was) return;
            const NSRect f = [win frame];
            std::fprintf(stderr,
                         "[present-view] t=%.3f window hwnd=0x%x on-screen %d->%d "
                         "guest_visible=%d frame=%.0f,%.0f %.0fx%.0f\n",
                         present_view_timestamp(),
                         static_cast<unsigned>(state.hwnd), was ? 1 : 0,
                         now ? 1 : 0, state.visible ? 1 : 0, f.origin.x,
                         f.origin.y, f.size.width, f.size.height);
            bridge.debug_describe_window_presentation_views(
                (__bridge void*)win, now ? "vis-on" : "vis-off");
        }
    } visibility_trace{*this, state, win, static_cast<bool>([win isVisible])};
    // MS Learn SetWindowRgn: empty region stays invisible. Off-monitor guest
    // rects (wizard pages parked past SM_CXSCREEN) also stay orderOut so
    // AppKit cannot clamp a second blank wizard onto the primary display.
    // A covering WS_POPUP that owns other top-level windows is the same
    // class of host: the owned dialogs paint the wizard; showing the host
    // on Next/Back is a black or chromeless duplicate.
    const HWND family_root =
        state.owner != NULL_HWND ? state.owner : NULL_HWND;
    // Suppress only the windows that view-hosting has taken over. Use the same
    // predicate as creation so a dialog that legitimately owns an NSWindow
    // (modal dialog of a captioned owner) is not silently kept off screen.
    const bool extra_family_window =
        state.is_dialog && family_root != NULL_HWND &&
        should_embed_owned_dialog(state.hwnd, state.guest_style, family_root);
    const bool on_monitor = guest_rect_intersects_any_monitor(state.frame);
    const bool effectively_visible =
        state.visible && !state.empty_window_rgn && on_monitor &&
        !composition_host_defers_to_owned(state) && !extra_family_window;
    if (effectively_visible) {
        elevate_window_above_fullscreen(state.hwnd, reinterpret_cast<void*>(win));
        // Undo any AppKit-side hiding of the window's controls — but never a
        // hide the guest asked for. A dialog that reflows itself calls
        // ShowWindow(SW_HIDE) on the controls it does not need (a second
        // CD-key row, an optional field); resurrecting those leaves a stray
        // control floating over the reflowed layout.
        NSView* content = [win contentView];
        for (NSView* sub in [content subviews]) {
            if ([sub isKindOfClass:[VtrContainerView class]]) {
                continue;
            }
            if (control_is_hidden_by_guest(sub)) {
                continue;
            }
            [sub setHidden:NO];
        }
        // Showing a window must not steal AppKit key status from a sibling
        // top-level (a browser window and its helper HWND). Repeated makeKeyAndOrderFront
        // on every visibility sync ping-pongs resign/become and WM_KILLFOCUS.
        //
        // The first transition to visible, or promotion from accessory to
        // a Dock application, may call makeKeyAndOrderFront.
        // Reclaiming key whenever `![win isKeyWindow]` fights the real frontmost
        // app (and pending-activation flush then calls activateIgnoringOtherApps
        // every pump tick) — that is the remaining single-HWND KillFocus loop.
        // Later activation is SetForegroundWindow / activate_window / user click.
        const HWND preferred = m_preferred_key_hwnd.load(std::memory_order_acquire);
        const bool first_show = ![win isVisible];
        const bool session_child = macos_session_is_child_process();
        const PresentationRecipe vis_recipe = make_presentation_recipe(
            make_presentation_signals(
                state.guest_style, state.guest_ex_style, state.is_dialog,
                state.client_drawn_frame, state.owner != NULL_HWND, false,
                state.fullscreen));
        // A child may activate its first eligible window without acquiring
        // another Dock tile. The session owns presentation identity.
        const bool owns_taskbar_window = presentation_has_taskbar_identity(
            make_presentation_signals(state.guest_style, state.guest_ex_style,
                state.is_dialog, state.client_drawn_frame,
                state.owner != NULL_HWND, false, state.fullscreen));
        const bool first_show_grant = session_child && owns_taskbar_window &&
            !m_used_child_first_show_grant;
        if (first_show_grant) m_used_child_first_show_grant = true;
        const bool may_become_key = first_show_takes_key(
            first_show, first_show_grant,
            state.show_without_activation, vis_recipe.activating,
            session_child, [NSApp isActive] == YES, owns_taskbar_window,
            state.hwnd == preferred || preferred == NULL_HWND);
        if (may_become_key) {
            [NSApp activateIgnoringOtherApps:YES];
            [win makeKeyAndOrderFront:nil];
            restack_owned_windows_above(state.hwnd);
            if (!state.is_dialog) {
                NSView* content = [win contentView];
                NSResponder* fr = [win firstResponder];
                const bool fr_in_window =
                    fr == static_cast<NSResponder*>(win) ||
                    (fr != nil && [fr isKindOfClass:[NSView class]] &&
                     [static_cast<NSView*>(fr) window] == win);
                if (content && !fr_in_window) {
                    [win makeFirstResponder:content];
                }
            }
            if ([win isKeyWindow]) {
                on_native_window_became_key(state.hwnd);
            } else {
                m_pending_activation_hwnd.store(state.hwnd,
                                                std::memory_order_release);
            }
        } else if (session_child) {
            // WS_VISIBLE must stay on screen without activating this accessory
            // NSApp. orderFront is a no-op while inactive; orderFrontRegardless
            // is the ShowWindow display path for CreateProcess descendants.
            // A re-sync of a window already on screen while another app is
            // active must not raise it over that app.
            if (visibility_sync_may_raise(first_show, [NSApp isActive] == YES)) {
                [win orderFrontRegardless];
            }
        } else {
            [win orderFront:nil];
        }
        // ShowWindow must display the window (Learn: SW_SHOW / SW_SHOWNORMAL).
        // makeKeyAndOrderFront is a no-op when Launch Services has not granted
        // activation (a launch that was not activated, or another app frontmost). The window
        // then stays orderOut, which is a Dock tile with no HWND on screen.
        if (![win isVisible]) {
            [win orderFrontRegardless];
        }
        // A game a launcher starts is a child process. AppKit often shows
        // that window without granting key status, so the guest never gets
        // WM_ACTIVATE / WM_SETFOCUS. The player then treats itself as
        // unfocused: it drops mouse, keyboard and gamepad input, and it
        // pauses the audio listener. Tell the guest this window is foreground
        // once it is actually on screen. A sibling top-level that is still
        // the preferred key window keeps it across a resign gap: this
        // fallback is for a window nothing else claims, not a key steal.
        if (owns_taskbar_window && vis_recipe.activating && [win isVisible] &&
            active_hwnd() == NULL_HWND &&
            (preferred == NULL_HWND || preferred == state.hwnd) &&
            visibility_sync_may_raise(first_show, [NSApp isActive] == YES)) {
            [NSApp activateIgnoringOtherApps:YES];
            if ([win canBecomeKeyWindow] && ![win isKeyWindow]) {
                [win makeKeyAndOrderFront:nil];
            }
            on_native_window_became_key(state.hwnd);
        }
        // An exclusive / borderless Space requested while the window was
        // ordered out forms once it is on screen.
        if (state.fullscreen && [win isVisible]) {
            reconcile_space_fullscreen(state.hwnd);
        }
    } else {
        // Owned wizard pages are hosted as container views in this window.
        // Ordering it out on HideWindow of the host HWND drops those pages
        // (and flashes a second chromeless NSWindow when they are shown).
        bool hosts_pages = false;
        NSView* content = [win contentView];
        for (NSView* sub in [content subviews]) {
            if ([sub isKindOfClass:[VtrContainerView class]]) {
                hosts_pages = true;
                break;
            }
        }
        if (hosts_pages) {
            NSView* content = [win contentView];
            for (NSView* sub in [content subviews]) {
                if (![sub isKindOfClass:[VtrContainerView class]]) {
                    [sub setHidden:YES];
                }
            }
            return;
        }
        [win orderOut:nil];
        if (state.visible && !state.empty_window_rgn && !on_monitor) {
            VITREO_TRACE(
                "[cocoa] hwnd=0x%x visible but off-monitor "
                "(%d,%d)-(%d,%d) — orderOut (parked capture dialog)\n",
                state.hwnd, state.frame.left, state.frame.top,
                state.frame.right, state.frame.bottom);
        }
    }
}

CocoaBridge::MonitorInfo CocoaBridge::get_primary_monitor_info() const {
    auto all = get_all_monitor_info();
    for (const auto& mi : all) {
        if (mi.primary) return mi;
    }
    if (!all.empty()) return all.front();
    return MonitorInfo{};
}

namespace {

/// Bumped whenever the display configuration changes. A CoreGraphics callback
/// rather than an NSApplication notification so it needs no run loop and no
/// main thread: guest threads reach the monitor query directly through the
/// user32 thunks.
std::atomic<u32> g_display_generation{1};

void display_reconfig_cb(CGDirectDisplayID, CGDisplayChangeSummaryFlags,
                         void*) {
    // Bump on every phase, including kCGDisplayBeginConfigurationFlag: an
    // extra invalidation costs one re-enumeration, whereas reasoning about
    // which flag subset can move a screen risks serving a stale rectangle.
    g_display_generation.fetch_add(1, std::memory_order_release);
}

void ensure_display_reconfig_hook() {
    static const bool installed = [] {
        CGDisplayRegisterReconfigurationCallback(display_reconfig_cb, nullptr);
        return true;
    }();
    (void)installed;
}

} // namespace

std::vector<CocoaBridge::MonitorInfo> CocoaBridge::get_all_monitor_info() const {
    ensure_display_reconfig_hook();
    const u32 generation = g_display_generation.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(m_monitor_cache_mu);
        if (m_monitor_cache_generation == generation &&
            !m_monitor_cache.empty()) {
            return m_monitor_cache;
        }
    }

    std::vector<MonitorInfo> out;
    @autoreleasepool {
        NSArray<NSScreen*>* screens = [NSScreen screens];
        if (screens.count == 0) return out;
        const CGFloat top_y = cocoa_virtual_top_y();
        // The Windows primary monitor is the one at (0,0): the menu-bar
        // screen, [NSScreen screens][0]. [NSScreen mainScreen] is the screen
        // with the key window and moves with the user's focus.
        NSScreen* primary = [screens firstObject];
        out.reserve(screens.count);
        for (NSScreen* screen in screens) {
            out.push_back(monitor_from_nsscreen(
                screen, top_y, screen == primary));
        }
        // Ensure exactly one primary even if mainScreen is nil.
        bool any_primary = false;
        for (const auto& mi : out) any_primary = any_primary || mi.primary;
        if (!any_primary && !out.empty()) out.front().primary = true;
    }
    // An empty result is not cached: it means AppKit has no NSScreen yet
    // (headless / early init), and the next caller must ask again.
    if (!out.empty()) {
        std::lock_guard<std::mutex> lock(m_monitor_cache_mu);
        m_monitor_cache = out;
        m_monitor_cache_generation = generation;
    }
    return out;
}

CocoaBridge::VirtualDesktopInfo CocoaBridge::get_virtual_desktop_info() const {
    VirtualDesktopInfo info;
    const auto monitors = get_all_monitor_info();
    if (monitors.empty()) return info;
    i32 min_l = monitors[0].left;
    i32 min_t = monitors[0].top;
    i32 max_r = monitors[0].right;
    i32 max_b = monitors[0].bottom;
    for (const auto& mi : monitors) {
        min_l = std::min(min_l, mi.left);
        min_t = std::min(min_t, mi.top);
        max_r = std::max(max_r, mi.right);
        max_b = std::max(max_b, mi.bottom);
    }
    info.origin_x = min_l;
    info.origin_y = min_t;
    info.width = max_r - min_l;
    info.height = max_b - min_t;
    info.monitor_count = static_cast<i32>(monitors.size());
    return info;
}
} // namespace vitreo::windowing
