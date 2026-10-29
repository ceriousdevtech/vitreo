// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_window_lifecycle.mm
/// @brief Creating and destroying the native window behind an HWND.
///
/// CreateWindow builds the NSWindow or NSPanel, its content view and its
/// delegate from the window's styles; DestroyWindow and shutdown take them
/// down again, with the child views and overlays attached to them.
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
// Window Management
// =============================================================================

VoidResult CocoaBridge::create_window(HWND hwnd, const WindowData& data) {
    VoidResult result = VoidResult::ok();
    // A tooltip control owns no window of its own here: its bubble is a native
    // macOS tooltip panel driven by TTM_TRACKACTIVATE (see
    // cocoa_mouse_tracking.mm). Giving it an NSWindow as well put an unpainted
    // white popup on screen -- the control never draws into its own HWND, and
    // nothing else was going to either.
    {
        std::wstring lower = data.class_name;
        for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
        if (lower == L"tooltips_class32") {
            return result;
        }
    }
    // A WS_POPUP + WS_EX_TRANSPARENT top-level window is a click-through
    // overlay. A tooltip is one. It does get a native window: the
    // guest paints it, and with CreateDIBSection returning an intact 64-bit
    // bits pointer that paint now lands. What it must NOT get is AppKit's
    // opaque window plate (see apply_overlay_appearance), which is what showed
    // as an empty white rectangle for as long as the DIB pointer was corrupt
    // and nothing was ever drawn into it.
    //
    // VITREO_OVERLAY_WINDOW=0 suppresses the native window entirely, which is
    // only a diagnostic: it hides the tooltip rather than drawing it.
    if (guest_window_is_click_through_overlay(data.style, data.ex_style) &&
        !environment_flag_enabled("VITREO_OVERLAY_WINDOW", true)) {
        VITREO_TRACE("[cocoa] hwnd=0x%x click-through overlay -> no native "
                     "window\n", hwnd);
        return result;
    }
    vtr_run_on_main_sync(^{
        // Direct CocoaBridge users may not have the PE runner's early
        // process/menu setup. Preserve the same ordering before
        // sharedApplication is called.
        if (!m_app_initialized && !data.title.empty()) {
            set_process_application_name(to_nsstring(data.title));
        }
        ensure_app_initialized();

        const PresentationRecipe recipe = make_presentation_recipe(
            presentation_signals(data.style, data.ex_style, data.is_dialog,
                                 data.client_drawn_frame,
                                 data.owner != NULL_HWND, false, false));
        if ((recipe.host == PresentationHost::View || data.is_dialog) &&
            data.owner != NULL_HWND &&
            should_embed_owned_dialog(hwnd, data.style, data.owner)) {
            const HWND host = family_presentation_hwnd(data.owner);
            if (host != NULL_HWND && host != hwnd) {
                add_container_view(host, hwnd, data);
                return;
            }
        }

        const u64 cocoa_mask =
            recipe.chrome == PresentationChrome::Native
                ? map_style_to_cocoa(data.style)
                : guest_chrome_style_mask(data.style);

        // The Cocoa content rect should equal the Win32 client area.
        // The caller (WindowManager::create_window) has already computed it
        // in data.client_rect with the non-client overhead subtracted.
        i32 w = data.client_rect.right - data.client_rect.left;
        i32 h = data.client_rect.bottom - data.client_rect.top;
        if (w <= 0) w = 500;
        if (h <= 0) h = 380;

        VITREO_TRACE( "[cocoa] create_window hwnd=0x%x style=0x%x size=%dx%d\n",
                     hwnd, data.style, w, h);
        if (diag::win_diag()) {
            NSString* title_ns = to_nsstring(data.title);
            const char* title_c = title_ns ? [title_ns UTF8String] : "";
            std::fprintf(stderr,
                "[cocoa] create_window hwnd=0x%x title='%s' style=0x%x ex=0x%x "
                "outer=%d,%d %dx%d client=%dx%d\n",
                hwnd, title_c ? title_c : "",
                data.style, data.ex_style,
                data.rect.left, data.rect.top,
                data.rect.right - data.rect.left,
                data.rect.bottom - data.rect.top,
                w, h);
        }

        // Scale Win32 pixel dimensions for Retina displays
        CGFloat scale = kWin32PixelScale;
        CGFloat screenH = vtr_win32_flip_height();
        CGFloat content_w = static_cast<CGFloat>(w) * scale;
        CGFloat content_h = static_cast<CGFloat>(h) * scale;
        // The content view *is* the Win32 client area, so it must land at the
        // client-area screen origin (outer + NC insets), not at the outer
        // corner. Placing it at data.rect.left/top put the client where Win32
        // says the frame starts, so every guest client->screen conversion came
        // out one NC inset away from the pixels: one guest drew tooltips a
        // caption height below the cursor. See client_origin_to_desktop().
        CGFloat content_x =
            static_cast<CGFloat>(data.rect.left + data.client_offset.x) * scale;
        // Win32 Y is top-down; Cocoa content rect origin is bottom-left.
        CGFloat content_y =
            screenH -
            static_cast<CGFloat>(data.rect.top + data.client_offset.y) * scale -
            content_h;
        NSRect contentRect = NSMakeRect(content_x, content_y, content_w, content_h);

        NSWindow* window = nil;
        if (recipe.host == PresentationHost::Panel) {
            VtrPanel* panel = [[VtrPanel alloc]
                initWithContentRect:contentRect
                          styleMask:(NSWindowStyleMaskBorderless |
                                     NSWindowStyleMaskNonactivatingPanel)
                            backing:NSBackingStoreBuffered
                              defer:NO];
            panel.bridge = this;
            panel.allowOffscreenFrame =
                !guest_rect_intersects_any_monitor(data.rect);
            [panel setFloatingPanel:YES];
            [panel setBecomesKeyOnlyIfNeeded:YES];
            window = panel;
        } else {
            VtrWindow* doc = [[VtrWindow alloc]
                initWithContentRect:contentRect
                          styleMask:static_cast<NSUInteger>(cocoa_mask)
                            backing:NSBackingStoreBuffered
                              defer:NO];
            doc.bridge = this;
            doc.allowOffscreenFrame =
                !guest_rect_intersects_any_monitor(data.rect);
            window = doc;
        }

        [window setTitle:to_nsstring(data.title)];
        [window setReleasedWhenClosed:NO];

        constexpr u32 kWsExLayered = 0x00080000u;
        if (guest_window_is_click_through_overlay(data.style, data.ex_style)) {
            apply_overlay_appearance(window);
            VITREO_TRACE("[cocoa] hwnd=0x%x WS_EX_TRANSPARENT popup -> no "
                         "background plate\n", hwnd);
        } else if (recipe.clear_host) {
            apply_composition_host_appearance(window, true);
            VITREO_TRACE("[cocoa] hwnd=0x%x %s -> clear host (opaque hits)\n",
                         hwnd,
                         (data.ex_style & kWsExLayered) != 0
                             ? "WS_EX_LAYERED"
                             : "recipe");
        }
        apply_presentation_recipe(window, recipe);

        // Install a flipped content view so child controls use Win32
        // Y-down coordinates and survive VtrPixelView insertion later.
        VtrContentView* flipView = [[VtrContentView alloc]
            initWithFrame:[[window contentView] bounds]];
        flipView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        // Layer-back the content view so that the layer-hosting
        // VtrPixelView (added later) composites in the correct
        // z-order with non-layer-backed sibling subviews.
        flipView.wantsLayer = YES;
        // Wizard pages paint via Cocoa controls on a light face; an unset
        // layer background reads as black and shows as thick bars around
        // letterboxed pixel views after window grow. Borderless skin hosts
        // must stay clear so full-client billboard children show.
        if (recipe.clear_host && !data.is_dialog) {
            flipView.layer.backgroundColor = NSColor.blackColor.CGColor;
        } else {
            NSColor* plate = unpainted_plate_color(data.is_dialog, data.erase_color);
            flipView.layer.backgroundColor = plate.CGColor;
            if (!data.is_dialog &&
                !guest_window_is_click_through_overlay(data.style, data.ex_style)) {
                [window setBackgroundColor:plate];
            }
        }
        [window setContentView:flipView];
        apply_guest_click_through(window, data.ex_style);

        // Complete NSApp launch (deferred from ensure_app_initialized) now
        // that we have a window title for the process name.
        finish_app_launch(data.title);

        // Set up delegate for close button → WM_CLOSE and resize → WM_SIZE
        VtrWindowDelegate* delegate = [[VtrWindowDelegate alloc] init];
        delegate.hwnd   = hwnd;
        delegate.queue  = m_queue;
        delegate.bridge = this;
        [window setDelegate:delegate];

        NativeWindowState state;
        state.hwnd        = hwnd;
        state.visible     = data.visible;
        state.is_dialog   = data.is_dialog;
        state.guest_style = data.style;
        state.guest_ex_style = data.ex_style;
        state.erase_color = data.erase_color;
        state.owner       = data.owner;
        // CS_DBLCLKS = 0x0008. Windows only delivers double-click messages to
        // classes that ask for them; everything else just sees a second
        // WM_LBUTTONDOWN.
        state.wants_dblclk = (data.class_style & 0x0008u) != 0;
        state.title       = data.title;
        // NativeWindowState::frame is the CLIENT area in Win32 screen
        // coordinates: every reader adds a client-relative point to it.
        state.frame       = Rect{data.rect.left + data.client_offset.x,
                                 data.rect.top + data.client_offset.y,
                                 data.rect.left + data.client_offset.x + w,
                                 data.rect.top + data.client_offset.y + h};
        state.ns_window   = reinterpret_cast<void*>(window);
        state.ns_delegate = reinterpret_cast<void*>(delegate);

        m_windows[hwnd] = std::move(state);
        apply_space_fullscreen_collection_behavior(
            window, false, data.style, data.ex_style, data.is_dialog,
            data.client_drawn_frame);

        // Set macOS app name from the FIRST non-child window title only.
        // Subsequent windows (dialogs, popups) must not overwrite the app name.
        if (!m_app_name_set && !(data.style & WS::CHILD) && !data.title.empty()) {
            m_app_name_set = true;
            NSString* title_ns = to_nsstring(data.title);
            [[NSProcessInfo processInfo] setValue:title_ns forKey:@"processName"];
            // Also update the app menu if one already exists
            NSMenu* mainMenu = [NSApp mainMenu];
            if (mainMenu && [[mainMenu itemArray] count] > 0) {
                NSMenuItem* appItem = [[mainMenu itemArray] firstObject];
                [appItem setTitle:title_ns];
                if ([appItem hasSubmenu]) {
                    NSMenu* appMenu = [appItem submenu];
                    [appMenu setTitle:title_ns];
                    for (NSMenuItem* mi in [appMenu itemArray]) {
                        NSString* t = [mi title];
                        if ([t hasPrefix:@"Quit"]) {
                            [mi setTitle:[NSString stringWithFormat:@"Quit %@", title_ns]];
                        }
                    }
                }
            }
        }

        if (data.visible) {
            VITREO_TRACE( "[cocoa] Showing window hwnd=0x%x backingScale=%.1f\n",
                         hwnd, [window backingScaleFactor]);
            // Use sync_native_window_visibility so off-monitor parked dialogs
            // (wizard pages past SM_CXSCREEN) stay orderOut instead of
            // being clamped onto the primary display as a blank second window.
            sync_native_window_visibility(m_windows[hwnd]);
        }
        if (data.owner != NULL_HWND) {
            auto oit = m_windows.find(data.owner);
            if (oit != m_windows.end()) {
                sync_native_window_visibility(oit->second);
            }
        }
        {
            const HWND root =
                data.owner != NULL_HWND ? data.owner : hwnd;
            adopt_extra_dialogs_in_family(root);
        }
        if (diag_logs_enabled()) {
            fprintf(stderr, "[winshow] create_window hwnd=0x%x style=0x%x child=%d "
                    "visible=%d content=(%.0f,%.0f %dx%d)\n",
                    hwnd, data.style, (data.style & WS::CHILD) ? 1 : 0,
                    data.visible ? 1 : 0,
                    contentRect.origin.x, contentRect.origin.y, w, h);
        }
    });
    return result;
}

void CocoaBridge::destroy_window(HWND hwnd) {
    // Windows drops capture when the capturing window goes away. Without this
    // a stale capture would keep routing every mouse message to a dead HWND,
    // which is the same freeze the missing capture routing caused.
    if (m_mouse_capture_hwnd.load(std::memory_order_acquire) == hwnd) {
        m_mouse_capture_hwnd.store(NULL_HWND, std::memory_order_release);
    }
    m_mouse_tracking.erase(hwnd);
    hide_tooltip(hwnd);
    vtr_run_on_main_sync(^{
        finish_native_window_resize(hwnd);
        on_native_move_size(hwnd, false);
        auto it = m_windows.find(hwnd);
        if (it != m_windows.end()) {
            forget_common_control(hwnd);
            if (it->second.ns_window) {
                auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
                // Detach delegate so programmatic closes aren't blocked by windowShouldClose
                // (which is intended to intercept user clicks and translate to WM_CLOSE).
                if (it->second.ns_delegate)
                    ((VtrWindowDelegate*)it->second.ns_delegate).bridge = nullptr;
                [win setDelegate:nil];
                // orderOut + close: releasedWhenClosed is NO, but AppKit drops its
                // on-screen retain once the window is ordered out — enough to make
                // wizard dialogs disappear while the host process stays up.
                [win orderOut:nil];
                [win close];
                it->second.ns_window = nullptr;
            }
            it->second.ns_delegate = nullptr;
            it->second.button_handlers.clear();
            m_mnemonics.erase(hwnd);
            m_presentation_views.erase(hwnd);
            forget_presentation_view_state(hwnd);
            m_guest_pixel_views.erase(hwnd);
            m_input_owning_controls.erase(hwnd);
            {
                auto overlay = m_video_overlays.find(hwnd);
                if (overlay != m_video_overlays.end()) {
                    vtr_close_video_overlay_window(overlay->second);
                    m_video_overlays.erase(overlay);
                }
            }
            clear_active_hwnd(hwnd);
            on_native_occlusion_changed(hwnd, true);
            {
                HWND preferred = hwnd;
                m_preferred_key_hwnd.compare_exchange_strong(
                    preferred, NULL_HWND, std::memory_order_acq_rel);
            }
            {
                HWND pending = hwnd;
                m_pending_activation_hwnd.compare_exchange_strong(
                    pending, NULL_HWND, std::memory_order_acq_rel);
            }
            const HWND owner = it->second.owner;
            m_windows.erase(it);
            if (owner != NULL_HWND) {
                auto oit = m_windows.find(owner);
                if (oit != m_windows.end()) {
                    sync_native_window_visibility(oit->second);
                }
            }
            return;
        }

        // Child/container HWNDs are tracked as NSView in m_controls.
        // Remove the view hierarchy so destroyed child dialogs do not remain visible.
        auto ctrl_it = m_controls.find(hwnd);
        if (ctrl_it == m_controls.end()) {
            if (diag::win_diag())
                fprintf(stderr, "[destroy] bridge: hwnd=0x%x NOT in m_controls (not removed)\n", hwnd);
            return;
        }
        if (diag::win_diag())
            fprintf(stderr, "[destroy] bridge: removing control view hwnd=0x%x\n", hwnd);

        NSView* root = (__bridge NSView*)ctrl_it->second;

        // Collect descendant HWNDs before removing views.
        std::vector<HWND> descendants;
        descendants.reserve(16);
        for (const auto& [candidate_hwnd, candidate_ptr] : m_controls) {
            if (candidate_hwnd == hwnd || candidate_ptr == nullptr) {
                continue;
            }
            NSView* candidate = (__bridge NSView*)candidate_ptr;
            NSView* ancestor = [candidate superview];
            while (ancestor != nil) {
                if (ancestor == root) {
                    descendants.push_back(candidate_hwnd);
                    break;
                }
                ancestor = [ancestor superview];
            }
        }

        [root removeFromSuperview];

        forget_common_control(hwnd);
        m_controls.erase(hwnd);

        m_controls_hidden_by_guest.erase(hwnd);
        m_transparent_children.erase(hwnd);
        m_hosted_peer_children.erase(hwnd);
        m_hosted_peer_present_sized.erase(hwnd);
        m_hosted_peer_parents.erase(hwnd);
        m_hosted_peer_origins.erase(hwnd);
        m_presentation_views.erase(hwnd);
        forget_presentation_view_state(hwnd);
        m_guest_pixel_views.erase(hwnd);
        m_input_owning_controls.erase(hwnd);
        {
            auto overlay = m_video_overlays.find(hwnd);
            if (overlay != m_video_overlays.end()) {
                vtr_close_video_overlay_window(overlay->second);
                m_video_overlays.erase(overlay);
            }
        }
        m_control_state.erase(hwnd);
        m_control_order.erase(hwnd);
        m_tab_order.erase(hwnd);
        m_pending_static_mnemonic.erase(hwnd);
        m_listbox_items.erase(hwnd);
        m_listbox_selection.erase(hwnd);
        m_listview_items.erase(hwnd);
        m_tab_controls.erase(hwnd);
        for (HWND child_hwnd : descendants) {
            forget_common_control(child_hwnd);
        m_controls.erase(child_hwnd);
            m_controls_hidden_by_guest.erase(child_hwnd);
            m_transparent_children.erase(child_hwnd);
            m_hosted_peer_children.erase(child_hwnd);
            m_hosted_peer_present_sized.erase(child_hwnd);
            m_hosted_peer_parents.erase(child_hwnd);
            m_hosted_peer_origins.erase(child_hwnd);
            m_presentation_views.erase(child_hwnd);
            forget_presentation_view_state(child_hwnd);
            m_guest_pixel_views.erase(child_hwnd);
            m_input_owning_controls.erase(child_hwnd);
            {
                auto overlay = m_video_overlays.find(child_hwnd);
                if (overlay != m_video_overlays.end()) {
                    vtr_close_video_overlay_window(overlay->second);
                    m_video_overlays.erase(overlay);
                }
            }
            m_control_state.erase(child_hwnd);
            m_control_order.erase(child_hwnd);
            m_tab_order.erase(child_hwnd);
            m_pending_static_mnemonic.erase(child_hwnd);
            m_listbox_items.erase(child_hwnd);
            m_listbox_selection.erase(child_hwnd);
            m_listview_items.erase(child_hwnd);
            m_tab_controls.erase(child_hwnd);
        }
    });
}

void CocoaBridge::destroy_all_windows() {
    // Snapshot under the caller's thread, then tear down on AppKit. Guest
    // crash/exit paths often invoke this from a non-main worker.
    vtr_run_on_main_sync(^{
        @autoreleasepool {
            std::vector<HWND> top_level;
            top_level.reserve(m_windows.size());
            for (const auto& [hwnd, state] : m_windows) {
                (void)state;
                top_level.push_back(hwnd);
            }
            for (HWND hwnd : top_level) {
                auto it = m_windows.find(hwnd);
                if (it == m_windows.end()) {
                    continue;
                }
                forget_common_control(hwnd);
                if (it->second.ns_window) {
                    auto* win = reinterpret_cast<NSWindow*>(it->second.ns_window);
                    if (it->second.ns_delegate)
                        ((VtrWindowDelegate*)it->second.ns_delegate).bridge = nullptr;
                    [win setDelegate:nil];
                    [win orderOut:nil];
                    [win close];
                    it->second.ns_window = nullptr;
                }
                it->second.ns_delegate = nullptr;
                it->second.button_handlers.clear();
            }
            m_windows.clear();
            m_mnemonics.clear();

            // Detach every control view (dialog children, skin hosts, etc.).
            std::vector<std::pair<HWND, void*>> controls(
                m_controls.begin(), m_controls.end());
            for (const auto& [hwnd, ptr] : controls) {
                (void)hwnd;
                if (!ptr) {
                    continue;
                }
                NSView* view = (__bridge NSView*)ptr;
                [view removeFromSuperview];
            }
            for (const auto& [hwnd, ptr] : controls) forget_common_control(hwnd);
            m_controls.clear();
            m_controls_hidden_by_guest.clear();
            m_presentation_views.clear();
            m_guest_pixel_views.clear();
            m_input_owning_controls.clear();
            for (auto& [overlay_hwnd, overlay_ptr] : m_video_overlays) {
                (void)overlay_hwnd;
                vtr_close_video_overlay_window(overlay_ptr);
            }
            m_video_overlays.clear();
            m_control_state.clear();
            m_control_order.clear();
            m_tab_order.clear();
            m_pending_static_mnemonic.clear();
            m_listbox_items.clear();
            m_listbox_selection.clear();
            m_listview_items.clear();
            m_tab_controls.clear();

            VITREO_TRACE("[cocoa] destroy_all_windows: closed %zu top-level "
                         "window(s)\n",
                         top_level.size());
        }
    });
}

} // namespace vitreo::windowing
