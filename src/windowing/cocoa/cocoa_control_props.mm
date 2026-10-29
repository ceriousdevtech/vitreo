// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_control_props.mm
/// @brief Reading and writing a control's properties after it exists.
///
/// Text, enabled state, font, button check state, progress range/value and
/// marquee mode, plus the VFS folder picker — which is here because it is built
/// out of a tree control rather than out of NSOpenPanel: the guest browses the
/// capsule filesystem, and a native panel would show the host's.
///
/// These are the setters a guest reaches through SendMessage, so each one lands
/// as a Win32 message code the caller chose, not as a typed API. The mapping
/// from that code to an AppKit property is the substance of the file.
///
/// Split out of cocoa_bridge.mm. Contiguous, and closed except for two helpers
/// now declared in objc_helpers.h.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_control_delegates.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/cocoa/app_icon_presentation.hpp"
#include "windowing/model/diag_flags.hpp"
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
// Set Control Text
// =============================================================================

bool CocoaBridge::set_control_text(HWND ctrl_hwnd, const std::wstring& text) {
    if (![NSThread isMainThread]) {
        std::wstring text_copy = text;
        __block bool ok = false;
        vtr_run_on_main_sync(^{
            ok = this->set_control_text(ctrl_hwnd, text_copy);
        });
        return ok;
    }

    // Treat top-level window HWNDs as window titles (SetWindowTextW).
    auto win_it = m_windows.find(ctrl_hwnd);
    if (win_it != m_windows.end()) {
        if (!win_it->second.ns_window) {
            return false;
        }
        auto* win = reinterpret_cast<NSWindow*>(win_it->second.ns_window);
        NSString* title_ns = to_nsstring(text);
        [win setTitle:title_ns];

        // If the macOS app-menu name hasn't been set yet (dialog created
        // with empty title, then filled via SetWindowTextW), apply it now.
        // Only use windows with a title bar (NSWindowStyleMaskTitled) to
        // avoid splash/progress windows setting the app name to transient
        // status text like "verifying installer: 10%".
        if (!m_app_name_set && !text.empty() &&
            ([win styleMask] & NSWindowStyleMaskTitled)) {
            m_app_name_set = true;
            [[NSProcessInfo processInfo] setValue:title_ns forKey:@"processName"];
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
        return true;
    }

    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        VITREO_TRACE( "[cocoa] set_control_text: control 0x%x not found\n", ctrl_hwnd);
        return false;
    }

    NSView* view = (__bridge NSView*)it->second;
    NSString* ns_text = to_nsstring(text);
    auto state_it = m_control_state.find(ctrl_hwnd);
    unichar accel_char = 0;
    NSRange accel_range = NSMakeRange(NSNotFound, 0);
    if (state_it != m_control_state.end() &&
        state_it->second.class_ordinal == 0x0082 &&
        !(state_it->second.style & 0x00000080)) {
        ns_text = strip_win_accelerator(ns_text, &accel_char, &accel_range);
        if (accel_char != 0) {
            const auto& order = m_control_order[state_it->second.parent];
            auto current = std::find(order.begin(), order.end(), ctrl_hwnd);
            constexpr u32 WS_TABSTOP_STYLE = 0x00010000;
            for (++current; current != order.end(); ++current) {
                auto target_state = m_control_state.find(*current);
                if (target_state != m_control_state.end() &&
                    (target_state->second.style & WS_TABSTOP_STYLE)) {
                    m_mnemonics[state_it->second.owner][static_cast<u32>(towlower(accel_char))] = *current;
                    break;
                }
            }
        }
    }

    if (diag::win_diag())
        fprintf(stderr, "[settext] hwnd=0x%x text='%s'\n", ctrl_hwnd, [ns_text UTF8String]);
    VITREO_TRACE( "[cocoa] set_control_text: hwnd=0x%x text='%s'\n",
                 ctrl_hwnd, [ns_text UTF8String]);

    // Drop any guest-painted overlay so native control text is visible.
    // Measure-only GetDC/ReleaseDC used to flush a blank framebuffer above
    // NSTextField/NSButton and hide SetWindowText updates (a destination-path
    // static).
    {
        auto pixel_it = m_guest_pixel_views.find(ctrl_hwnd);
        if (pixel_it != m_guest_pixel_views.end() && pixel_it->second) {
            NSView* overlay = (__bridge NSView*)pixel_it->second;
            [overlay removeFromSuperview];
            m_guest_pixel_views.erase(pixel_it);
            VITREO_TRACE(
                "[cocoa] set_control_text: removed guest overlay hwnd=0x%x\n",
                ctrl_hwnd);
        }
    }

    if ([view isKindOfClass:[NSTextField class]]) {
        [(NSTextField*)view setStringValue:ns_text];
        underline_win_accelerator((NSControl*)view, accel_range);
    } else if (NSTextView* tv = vtr_document_text_view(view)) {
        [tv setString:ns_text ?: @""];
    } else if ([view isKindOfClass:[NSButton class]]) {
        NSRange button_accel_range = NSMakeRange(NSNotFound, 0);
        NSString* button_title = strip_win_accelerator(ns_text, &accel_char,
                                                       &button_accel_range);
        [(NSButton*)view setTitle:button_title];
        underline_win_accelerator((NSControl*)view, button_accel_range);
    } else if ([view isKindOfClass:[NSBox class]]) {
        [(NSBox*)view setTitle:strip_win_accelerator(ns_text)];
    }
    [view setNeedsDisplay:YES];

    return true;
}

bool CocoaBridge::set_control_rich_content(HWND ctrl_hwnd, const u8* data, size_t len,
                                           bool is_rtf) {
    if (![NSThread isMainThread]) {
        std::vector<u8> copy(data, data + len);
        __block bool ok = false;
        vtr_run_on_main_sync(^{
            ok = this->set_control_rich_content(
                ctrl_hwnd, copy.data(), copy.size(), is_rtf);
        });
        return ok;
    }

    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return false;
    }
    NSView* view = (__bridge NSView*)it->second;
    NSTextView* tv = vtr_document_text_view(view);
    if (!tv) {
        return false;
    }

    NSData* nsdata = [NSData dataWithBytes:data length:len];
    if (is_rtf) {
        NSDictionary* attrs = nil;
        NSAttributedString* attr =
            [[NSAttributedString alloc] initWithRTF:nsdata documentAttributes:&attrs];
        if (!attr) {
            // Some streams are UTF-8/ANSI text with an .rtf name; fall through.
            NSString* plain = [[NSString alloc] initWithData:nsdata
                                                   encoding:NSUTF8StringEncoding];
            if (!plain) {
                plain = [[NSString alloc] initWithData:nsdata
                                              encoding:NSWindowsCP1252StringEncoding];
            }
            [tv setString:plain ?: @""];
            [tv setTextColor:[NSColor textColor]];
        } else {
            [[tv textStorage] setAttributedString:attr];
            // Force a readable color in case the RTF color table is white-on-white
            // under the host appearance.
            [tv setTextColor:[NSColor textColor]];
        }
    } else {
        NSString* plain = [[NSString alloc] initWithData:nsdata
                                               encoding:NSUTF8StringEncoding];
        if (!plain) {
            plain = [[NSString alloc] initWithData:nsdata
                                          encoding:NSWindowsCP1252StringEncoding];
        }
        if (!plain && len >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
            plain = [[NSString alloc] initWithBytes:data + 2
                                             length:len - 2
                                           encoding:NSUTF16LittleEndianStringEncoding];
        }
        [tv setString:plain ?: @""];
        [tv setTextColor:[NSColor textColor]];
    }
    [tv setNeedsDisplay:YES];
    [view setNeedsDisplay:YES];
    VITREO_TRACE("[cocoa] set_control_rich_content hwnd=0x%x bytes=%zu rtf=%d\n",
                 ctrl_hwnd, len, is_rtf ? 1 : 0);
    // After loading EULA text, fire scroll notifications so Agree can enable
    // when content already fits (or so the dialog re-queries GetScrollInfo).
    notify_richedit_scroll(ctrl_hwnd);
    return true;
}

bool CocoaBridge::set_control_scroll_pos(HWND ctrl_hwnd, i32 x, i32 y) {
    __block bool ok = false;
    auto body = ^{
        auto it = m_controls.find(ctrl_hwnd);
        if (it == m_controls.end()) return;
        NSView* view = (__bridge NSView*)it->second;
        if (![view isKindOfClass:[NSScrollView class]]) return;
        NSScrollView* scroll = (NSScrollView*)view;
        NSView* doc = [scroll documentView];
        if (!doc) return;
        NSPoint pt = NSMakePoint(static_cast<CGFloat>(x), static_cast<CGFloat>(y));
        [doc scrollPoint:pt];
        ok = true;
        notify_richedit_scroll(ctrl_hwnd);
    };
    if ([NSThread isMainThread]) body();
    else vtr_run_on_main_sync(body);
    return ok;
}

bool CocoaBridge::get_control_scroll_pos(HWND ctrl_hwnd, i32& x, i32& y) const {
    ScrollInfoOut info{};
    if (!get_control_scroll_info(ctrl_hwnd, info)) {
        return false;
    }
    x = 0;
    y = info.n_pos;
    return true;
}

bool CocoaBridge::set_edit_selection(HWND ctrl_hwnd, i32 start, i32 end) {
    __block bool ok = false;
    auto body = ^{
        auto it = m_controls.find(ctrl_hwnd);
        if (it == m_controls.end()) return;
        NSView* view = (__bridge NSView*)it->second;

        auto apply_to_text = ^(NSText* text) {
            if (!text) return;
            NSString* s = [text string];
            const NSInteger len = static_cast<NSInteger>([s length]);
            NSInteger a = start < 0 ? 0 : static_cast<NSInteger>(start);
            NSInteger b = end < 0 ? len : static_cast<NSInteger>(end);
            if (a > len) a = len;
            if (b > len) b = len;
            if (a > b) std::swap(a, b);
            [text setSelectedRange:NSMakeRange(static_cast<NSUInteger>(a),
                                               static_cast<NSUInteger>(b - a))];
            ok = true;
        };

        if (NSTextView* tv = vtr_document_text_view(view)) {
            apply_to_text(tv);
            return;
        }
        if (![view isKindOfClass:[NSTextField class]]) return;
        NSTextField* field = (NSTextField*)view;
        NSWindow* win = [field window];
        if (!win) return;
        // Ensure the field editor exists so selection applies to the live edit
        // session (a wizard field's placeholder select-all on focus).
        [win makeFirstResponder:field];
        NSText* editor = [win fieldEditor:YES forObject:field];
        if (editor) {
            apply_to_text(editor);
        } else if (start <= 0 && end < 0) {
            [field selectText:nil];
            ok = true;
        }
        VITREO_TRACE("[cocoa] set_edit_selection hwnd=0x%x start=%d end=%d ok=%d\n",
                     ctrl_hwnd, start, end, ok ? 1 : 0);
    };
    if ([NSThread isMainThread]) body();
    else vtr_run_on_main_sync(body);
    return ok;
}

void CocoaBridge::notify_richedit_scroll(HWND ctrl_hwnd) {
    auto state_it = m_control_state.find(ctrl_hwnd);
    if (state_it == m_control_state.end()) {
        return;
    }
    HWND parent = state_it->second.parent;
    u32 ctrl_id = state_it->second.ctrl_id;

    constexpr u32 EN_VSCROLL = 0x0602u;
    constexpr u32 WM_VSCROLL = 0x0115u;
    constexpr u32 SB_BOTTOM = 7u;
    constexpr u32 SB_THUMBTRACK = 5u;

    ScrollInfoOut scroll{};
    const bool have = get_control_scroll_info(ctrl_hwnd, scroll);
    const bool at_bottom =
        have &&
        (scroll.n_max <= static_cast<i32>(scroll.n_page) - 1 ||
         scroll.n_pos >= scroll.n_max - static_cast<i32>(scroll.n_page) + 1);

    post_window_message(
        parent, WM::COMMAND,
        (static_cast<u64>(EN_VSCROLL) << 16) | (ctrl_id & 0xFFFFu),
        static_cast<u64>(ctrl_hwnd));
    post_window_message(
        parent, WM_VSCROLL,
        at_bottom ? SB_BOTTOM : SB_THUMBTRACK,
        static_cast<u64>(ctrl_hwnd));
    post_window_message(
        ctrl_hwnd, WM_VSCROLL,
        at_bottom ? SB_BOTTOM : SB_THUMBTRACK, 0);

    VITREO_TRACE(
        "[cocoa] richedit scroll notify parent=0x%x ctrl=0x%x id=%u "
        "pos=%d max=%d page=%u at_bottom=%d\n",
        parent, ctrl_hwnd, ctrl_id, scroll.n_pos, scroll.n_max, scroll.n_page,
        at_bottom ? 1 : 0);
}

bool CocoaBridge::get_control_scroll_info(HWND ctrl_hwnd, ScrollInfoOut& out) const {
    __block bool ok = false;
    __block ScrollInfoOut local{};
    auto body = ^{
        auto it = m_controls.find(ctrl_hwnd);
        if (it == m_controls.end()) {
            return;
        }
        NSView* view = (__bridge NSView*)it->second;
        NSScrollView* scroll = nil;
        if ([view isKindOfClass:[NSScrollView class]]) {
            scroll = (NSScrollView*)view;
        }
        if (!scroll) {
            return;
        }
        NSView* doc = [scroll documentView];
        if (!doc) {
            return;
        }
        // Force text layout so usedRect reflects the full EULA height.
        if ([doc isKindOfClass:[NSTextView class]]) {
            NSTextView* tv = (NSTextView*)doc;
            NSLayoutManager* lm = [tv layoutManager];
            NSTextContainer* tc = [tv textContainer];
            if (lm && tc) {
                [lm ensureLayoutForTextContainer:tc];
                const NSRect used = [lm usedRectForTextContainer:tc];
                const CGFloat need_h =
                    std::max(NSMaxY(used), [scroll contentSize].height);
                NSRect fr = [tv frame];
                if (fr.size.height < need_h) {
                    fr.size.height = need_h;
                    [tv setFrame:fr];
                }
            }
        }

        const NSRect vis = [scroll documentVisibleRect];
        const CGFloat doc_h = std::max<CGFloat>(1.0, [doc frame].size.height);
        const CGFloat vis_h = std::max<CGFloat>(1.0, vis.size.height);
        // Win32 SCROLLINFO: nPos ranges from nMin .. nMax-(nPage-1).
        i32 n_min = 0;
        u32 n_page = static_cast<u32>(
            std::max<i32>(1, static_cast<i32>(std::lround(vis_h))));
        i32 n_max = std::max(0, static_cast<i32>(std::lround(doc_h)) - 1);
        i32 n_pos = static_cast<i32>(std::lround(vis.origin.y));
        if (n_max < static_cast<i32>(n_page) - 1) {
            // Content fits — already at end (Agree should enable without scrolling).
            n_max = static_cast<i32>(n_page) - 1;
            n_pos = 0;
        } else {
            const i32 max_pos = n_max - static_cast<i32>(n_page) + 1;
            if (n_pos > max_pos) n_pos = max_pos;
            if (n_pos < 0) n_pos = 0;
            // Near-bottom slack for AppKit rounding / scroller thumb.
            if (vis.origin.y + vis_h >= doc_h - 4.0) {
                n_pos = max_pos;
            }
        }
        local = ScrollInfoOut{n_min, n_max, n_page, n_pos, n_pos};
        ok = true;
    };
    if ([NSThread isMainThread]) {
        body();
    } else {
        vtr_run_on_main_sync(body);
    }
    if (ok) {
        out = local;
    }
    return ok;
}

bool CocoaBridge::activate_native_mnemonic(void* ns_window, u32 character) {
    if (![NSThread isMainThread]) return false;
    HWND owner = find_hwnd_for_window(ns_window);
    auto owner_it = m_mnemonics.find(owner);
    if (owner_it == m_mnemonics.end()) return false;
    auto binding = owner_it->second.find(static_cast<u32>(towlower(static_cast<wint_t>(character))));
    if (binding == owner_it->second.end()) return false;
    auto control = m_controls.find(binding->second);
    if (control == m_controls.end()) return false;

    NSView* view = (__bridge NSView*)control->second;
    if ([view isHidden] || ([view respondsToSelector:@selector(isEnabled)] &&
                            ![(NSControl*)view isEnabled])) {
        return false;
    }
    if ([view isKindOfClass:[NSButton class]]) {
        [(NSButton*)view performClick:nil];
    } else {
        [(NSWindow*)ns_window makeFirstResponder:view];
    }
    return true;
}

// =============================================================================
// Set Control Enabled
// =============================================================================

bool CocoaBridge::set_control_enabled(HWND ctrl_hwnd, bool enable) {
    // Check top-level windows first
    auto win_it = m_windows.find(ctrl_hwnd);
    if (win_it != m_windows.end()) {
        NSWindow* window = reinterpret_cast<NSWindow*>(win_it->second.ns_window);
        vtr_run_on_main_sync(^{
            [window setIgnoresMouseEvents:!enable];
        });
        return true;
    }

    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return false;
    }

    NSView* view = (__bridge NSView*)it->second;

    void (^updateBlock)(void) = ^{
        if ([view isKindOfClass:[NSControl class]]) {
            // NSButton, NSTextField, NSComboBox etc. support setEnabled directly
            [(NSControl*)view setEnabled:enable];
        } else if (NSTextView* tv = vtr_document_text_view(view)) {
            [tv setEditable:enable];
            [tv setSelectable:enable];
            [tv setTextColor:enable ? [NSColor textColor] : [NSColor disabledControlTextColor]];
            [view setAlphaValue:enable ? 1.0 : 0.7];
        } else {
            // Non-NSControl views (labels, boxes): use alpha to indicate disabled
            [view setAlphaValue:enable ? 1.0 : 0.4];
        }
        [view setNeedsDisplay:YES];
    };

    // SendMessage is synchronous. Keep the native mutation synchronous too:
    // installers commonly update controls from a guest worker while the host
    // main thread is running Vitreo's nested AppKit loop. A main-queue async
    // block can then remain queued until installation ends, so the label moves
    // while the bar appears frozen.
    vtr_run_on_main_sync(updateBlock);

    return true;
}

void CocoaBridge::restack_owned_windows_above(HWND owner_hwnd) {
    if (owner_hwnd == NULL_HWND) return;

    // Re-entrancy: transferring key to an owned dialog re-enters becomeKey.
    static thread_local int depth = 0;
    if (depth > 0) return;
    struct DepthGuard {
        int& d;
        DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; }
    } guard{depth};

    auto owner_it = m_windows.find(owner_hwnd);
    if (owner_it == m_windows.end() || !owner_it->second.ns_window) return;
    NSWindow* owner_win =
        reinterpret_cast<NSWindow*>(owner_it->second.ns_window);

    HWND prefer_key = NULL_HWND;
    NSWindow* prefer_win = nil;
    int raised = 0;
    for (auto& [hwnd, state] : m_windows) {
        if (state.owner != owner_hwnd || hwnd == owner_hwnd) continue;
        if (!state.visible || state.empty_window_rgn || !state.ns_window) continue;
        if (!guest_rect_intersects_any_monitor(state.frame)) continue;
        NSWindow* owned = reinterpret_cast<NSWindow*>(state.ns_window);
        // orderWindow: and makeKeyAndOrderFront both order a window in.
        // During wizard Next/Back several owned HWNDs are WS_VISIBLE at once
        // (outgoing page, incoming page, BitBlt target). Raising every one
        // presents the previous/next page as a second chromeless NSWindow.
        // Z-order only windows AppKit already has on screen; ShowWindow is
        // what orders a HWND in.
        if (![owned isVisible]) continue;
        // Match Win32: owned top-level windows stay strictly above the owner.
        [owned orderWindow:NSWindowAbove relativeTo:[owner_win windowNumber]];
        ++raised;
        if (prefer_key == NULL_HWND || state.is_dialog) {
            prefer_key = hwnd;
            prefer_win = owned;
        }
    }
    if (raised == 0 || !prefer_win) return;

    elevate_window_above_fullscreen(prefer_key, prefer_win);
    VITREO_TRACE("[cocoa] restack_owned_windows_above owner=0x%x raised=%d "
                 "top=0x%x\n",
                 owner_hwnd, raised, prefer_key);
}

void CocoaBridge::elevate_window_above_fullscreen(HWND hwnd, void* ns_window) {
    if (!ns_window) return;

    // Find the highest level among guest windows currently in game-style
    // full-screen. Child/control windows are NSViews, so only top-level guest
    // windows appear here.
    NSInteger fullscreen_level = NSNormalWindowLevel;
    bool any_fullscreen = false;
    for (const auto& [other_hwnd, state] : m_windows) {
        if (other_hwnd == hwnd || !state.fullscreen || !state.ns_window) continue;
        NSWindow* other = reinterpret_cast<NSWindow*>(state.ns_window);
        fullscreen_level = std::max(fullscreen_level, [other level]);
        any_fullscreen = true;
    }
    NSWindow* window = reinterpret_cast<NSWindow*>(ns_window);
    auto it = m_windows.find(hwnd);
    const bool topmost =
        it != m_windows.end() &&
        (it->second.guest_ex_style & 0x00000008u) != 0;
    const bool in_space =
        (it != m_windows.end() && it->second.fullscreen) ||
        ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
    const HostLevel level = host_window_level(
        topmost, in_space, any_fullscreen, [NSApp isActive] == YES);
    if (level == HostLevel::Normal) {
        const bool was_raised =
            it != m_windows.end() && it->second.level_elevated;
        if ((in_space || was_raised) &&
            [window level] != NSNormalWindowLevel) {
            [window setLevel:NSNormalWindowLevel];
        }
        if (it != m_windows.end()) it->second.level_elevated = false;
        return;
    }
    if (level == HostLevel::AboveFullscreen) {
        [window setCollectionBehavior:[window collectionBehavior] |
                                      NSWindowCollectionBehaviorFullScreenAuxiliary];
        [window setLevel:fullscreen_level + 1];
        if (it != m_windows.end()) it->second.level_elevated = true;
        VITREO_TRACE("[cocoa] elevate_window_above_fullscreen hwnd=0x%x level=%ld\n",
                     hwnd, static_cast<long>(fullscreen_level + 1));
        return;
    }
    if (topmost) {
        [window setLevel:NSFloatingWindowLevel];
    }
}

void CocoaBridge::restore_elevated_window_levels() {
    for (auto& [hwnd, state] : m_windows) {
        if (!state.level_elevated || !state.ns_window) continue;
        NSWindow* window = reinterpret_cast<NSWindow*>(state.ns_window);
        const bool topmost = (state.guest_ex_style & 0x00000008u) != 0;
        [window setLevel:topmost ? NSFloatingWindowLevel : NSNormalWindowLevel];
        state.level_elevated = false;
        VITREO_TRACE("[cocoa] restore_elevated_window_levels hwnd=0x%x\n", hwnd);
    }
}

bool CocoaBridge::activate_window(HWND hwnd) {
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end() || !it->second.ns_window) {
        return false;
    }

    NSWindow* window = reinterpret_cast<NSWindow*>(it->second.ns_window);
    // Learn SetForegroundWindow: a process that is not the foreground one
    // (and was not started by it) cannot take the foreground; Windows
    // flashes the taskbar button instead and returns zero. Here: Dock
    // attention, no activation, no ordering (which would slide its Space
    // back over the app the user Cmd-Tabbed to).
    if (!process_may_set_foreground()) {
        // One flash per background period, not one per refused call.
        if (!m_foreground_refusal_flashed.exchange(true)) {
            request_user_attention(hwnd, true);
        }
        VITREO_TRACE("[cocoa] activate_window hwnd=0x%x refused: not the "
                     "foreground app\n", hwnd);
        return false;
    }
    // SetForegroundWindow / programmatic activation selects the sticky
    // preferred key top-level before AppKit finish; visibility sync for
    // siblings must not steal key during the transition.
    m_preferred_key_hwnd.store(hwnd, std::memory_order_release);
    vtr_run_on_main_sync(^{
        [NSApp activateIgnoringOtherApps:YES];
        elevate_window_above_fullscreen(hwnd, window);
        [window makeKeyAndOrderFront:nil];
        restack_owned_windows_above(hwnd);
        // SetForegroundWindow must bring a WS_VISIBLE window on screen
        // (Learn). makeKeyAndOrderFront is a no-op without activation
        // permission, which leaves a Dock tile and no HWND.
        auto live = m_windows.find(hwnd);
        if (live != m_windows.end() && live->second.visible &&
            ![window isVisible]) {
            [window orderFrontRegardless];
        }
        if ([window isKeyWindow]) {
            on_native_window_became_key(hwnd);
        } else {
            m_pending_activation_hwnd.store(hwnd, std::memory_order_release);
        }
    });
    return true;
}

// =============================================================================
// Set Progress Range
// =============================================================================

bool CocoaBridge::set_progress_range(HWND ctrl_hwnd, double min_val, double max_val) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        VITREO_TRACE( "[cocoa] set_progress_range: control 0x%x not found\n", ctrl_hwnd);
        return false;
    }

    NSView* view = (__bridge NSView*)it->second;
    if (![view isKindOfClass:[NSProgressIndicator class]]) {
        VITREO_TRACE( "[cocoa] set_progress_range: control 0x%x is not a progress bar\n", ctrl_hwnd);
        return false;
    }

    NSProgressIndicator* progress = (NSProgressIndicator*)view;

    VITREO_TRACE( "[cocoa] set_progress_range: hwnd=0x%x min=%.0f max=%.0f\n",
                 ctrl_hwnd, min_val, max_val);

    void (^updateBlock)(void) = ^{
        [progress setMinValue:min_val];
        [progress setMaxValue:max_val];
        [progress setNeedsDisplay:YES];
    };

    vtr_run_on_main_sync(updateBlock);

    return true;
}

// =============================================================================
// Set Progress Value
// =============================================================================

bool CocoaBridge::set_progress_value(HWND ctrl_hwnd, double position) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        VITREO_TRACE( "[cocoa] set_progress_value: control 0x%x not found\n", ctrl_hwnd);
        return false;
    }

    NSView* view = (__bridge NSView*)it->second;
    if (![view isKindOfClass:[NSProgressIndicator class]]) {
        VITREO_TRACE( "[cocoa] set_progress_value: control 0x%x is not a progress bar\n", ctrl_hwnd);
        return false;
    }

    NSProgressIndicator* progress = (NSProgressIndicator*)view;

    VITREO_TRACE( "[cocoa] set_progress_value: hwnd=0x%x pos=%.0f\n",
                 ctrl_hwnd, position);

    void (^updateBlock)(void) = ^{
        // PBM_SETPOS implies determinate mode on real Windows. If the bar is
        // currently in marquee/indeterminate animation (e.g. set earlier this
        // session), stop the animation and switch back so the value is visible.
        if ([progress isIndeterminate]) {
            [progress stopAnimation:nil];
            [progress setIndeterminate:NO];
        }
        [progress setDoubleValue:position];
    };

    vtr_run_on_main_sync(updateBlock);

    return true;
}

// =============================================================================
// Progress Marquee (indeterminate) mode
// =============================================================================

bool CocoaBridge::set_progress_marquee(HWND ctrl_hwnd, bool enable) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        VITREO_TRACE("[cocoa] set_progress_marquee: control 0x%x not found\n", ctrl_hwnd);
        return false;
    }

    NSView* view = (__bridge NSView*)it->second;
    if (![view isKindOfClass:[NSProgressIndicator class]]) {
        VITREO_TRACE("[cocoa] set_progress_marquee: control 0x%x is not a progress bar\n", ctrl_hwnd);
        return false;
    }

    NSProgressIndicator* progress = (NSProgressIndicator*)view;

    VITREO_TRACE("[cocoa] set_progress_marquee: hwnd=0x%x enable=%d\n",
                 ctrl_hwnd, enable ? 1 : 0);

    void (^updateBlock)(void) = ^{
        if (enable) {
            [progress setIndeterminate:YES];
            [progress startAnimation:nil];
        } else {
            [progress stopAnimation:nil];
            [progress setIndeterminate:NO];
        }
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), updateBlock);
    }

    return true;
}

// =============================================================================
// Button Check State
// =============================================================================

bool CocoaBridge::set_button_state(HWND ctrl_hwnd, u32 check) {
    // BST_UNCHECKED 0, BST_CHECKED 1, BST_INDETERMINATE 2 (mixed when the
    // button allows it; WinUser.h BST_*).
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) return false;
    NSView* view = (__bridge NSView*)it->second;
    if (![view isKindOfClass:[NSButton class]]) return false;
    NSButton* button = (NSButton*)view;
    void (^updateBlock)(void) = ^{
        NSControlStateValue v = check == 0 ? NSControlStateValueOff : NSControlStateValueOn;
        if (check == 2 && [button allowsMixedState]) v = NSControlStateValueMixed;
        [button setState:v];
    };
    if ([NSThread isMainThread]) updateBlock();
    else dispatch_async(dispatch_get_main_queue(), updateBlock);
    return true;
}

bool CocoaBridge::set_button_check(HWND ctrl_hwnd, bool checked) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return false;
    }

    NSView* view = (__bridge NSView*)it->second;
    if (![view isKindOfClass:[NSButton class]]) {
        return false;
    }

    NSButton* button = (NSButton*)view;
    void (^updateBlock)(void) = ^{
        [button setState:(checked ? NSControlStateValueOn : NSControlStateValueOff)];
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), updateBlock);
    }

    // NB: macOS manages radio single-selection per native group; we keep Win32
    // groups independent by assigning a distinct click selector per WS_GROUP
    // group (see add_child_view), so no manual cross-group bookkeeping is needed.
    return true;
}

void CocoaBridge::begin_radio_group() {
    m_active_radio_group = ++m_radio_group_seq;
}

void CocoaBridge::apply_auto_button_click(HWND ctrl_hwnd) {
    auto state_it = m_control_state.find(ctrl_hwnd);
    if (state_it == m_control_state.end()) {
        return;
    }
    const NativeControlState& st = state_it->second;
    if (st.class_ordinal != 0x0080) {
        return;
    }
    const u32 btn_type = st.style & 0x0Fu;
    constexpr u32 BS_RADIOBUTTON = 0x04;
    constexpr u32 BS_AUTORADIOBUTTON = 0x09;

    if (btn_type == BS_AUTORADIOBUTTON || btn_type == BS_RADIOBUTTON) {
        // MS Learn BS_AUTORADIOBUTTON: check this button, clear others in group.
        set_button_check(ctrl_hwnd, true);
        if (st.radio_group != 0) {
            for (const auto& [hwnd, other] : m_control_state) {
                if (hwnd == ctrl_hwnd) continue;
                if (other.parent != st.parent) continue;
                if (other.radio_group != st.radio_group) continue;
                if (other.class_ordinal != 0x0080) continue;
                const u32 ot = other.style & 0x0Fu;
                if (ot == BS_AUTORADIOBUTTON || ot == BS_RADIOBUTTON) {
                    set_button_check(hwnd, false);
                }
            }
        }
        VITREO_TRACE(
            "[cocoa] apply_auto_button_click radio hwnd=0x%x id=%u group=%u -> checked\n",
            ctrl_hwnd, st.ctrl_id, st.radio_group);
        return;
    }

    // BS_AUTOCHECKBOX: AppKit already toggled NSButtonTypeSwitch before the
    // action runs; do not toggle again. Non-auto checkbox state is parent-owned.
}

bool CocoaBridge::try_get_button_check(HWND ctrl_hwnd, u32& out_checked) const {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end() || it->second == nullptr) {
        return false;
    }
    NSView* view = (__bridge NSView*)it->second;
    if (![view isKindOfClass:[NSButton class]]) {
        return false;
    }
    out_checked = get_button_check(ctrl_hwnd);
    return true;
}

u32 CocoaBridge::get_button_check(HWND ctrl_hwnd) const {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) return 0;

    NSView* view = (__bridge NSView*)it->second;
    if (![view isKindOfClass:[NSButton class]]) return 0;

    NSButton* button = (NSButton*)view;
    __block NSControlStateValue state = NSControlStateValueOff;

    if ([NSThread isMainThread]) {
        state = [button state];
    } else {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            state = [button state];
            dispatch_semaphore_signal(sem);
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        // Built without ARC: release the +1 from dispatch_semaphore_create so
        // the Mach semaphore behind it is returned to the kernel.
        dispatch_release(sem);
    }

    if (state == NSControlStateValueMixed) return 2;  // BST_INDETERMINATE
    return (state == NSControlStateValueOn) ? 1 : 0;
}

// =============================================================================
// Set Control Font
// =============================================================================

bool CocoaBridge::set_control_font(HWND ctrl_hwnd, float pt_size, u16 weight, bool italic) {
    return set_control_font_face(ctrl_hwnd, {}, pt_size, weight, italic);
}

bool CocoaBridge::set_control_font_face(HWND ctrl_hwnd, const std::u16string& face,
                                        float pt_size, u16 weight, bool italic) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) return false;

    NSView* view = (__bridge NSView*)it->second;

    // A guest face: the font GDI measured with (a VCL label sizes itself with
    // DrawText(DT_CALCRECT); a wider native font would clip its last line).
    // No face: the system font at the requested size and weight.
    NSFont* face_font = face.empty() ? nil : vtr_gdi_matching_font(face, pt_size, weight, italic);
    NSFontWeight nsWeight = (weight >= 700) ? NSFontWeightBold
                          : (weight >= 500) ? NSFontWeightMedium
                          : NSFontWeightRegular;

    NSFont* base_font = face_font ?: [NSFont systemFontOfSize:static_cast<CGFloat>(pt_size)
                                                       weight:nsWeight];
    NSFont* italic_font = (!face_font && italic && base_font)
        ? [[NSFontManager sharedFontManager] convertFont:base_font
                                             toHaveTrait:NSItalicFontMask]
        : nil;
    // Not __block: without ARC a block retains captured objects only when they
    // are plain (non-__block) variables, and the font is autoreleased.
    NSFont* const font = italic_font ? italic_font : base_font;

    if (!font) return false;

    void (^updateBlock)(void) = ^{
        if ([view isKindOfClass:[VtrSysLinkView class]]) {
            [(VtrSysLinkView*)view setLinkFont:font];
        } else if ([view isKindOfClass:[NSTextField class]]) {
            [(NSTextField*)view setFont:font];
        } else if (NSTextView* tv = vtr_document_text_view(view)) {
            [tv setFont:font];
        } else if ([view isKindOfClass:[NSButton class]]) {
            [(NSButton*)view setFont:font];
        } else if ([view isKindOfClass:[NSBox class]]) {
            [(NSBox*)view setTitleFont:font];
        }
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), updateBlock);
    }

    return true;
}

// =============================================================================
// Get Control Text
// =============================================================================

std::wstring CocoaBridge::get_control_text(HWND ctrl_hwnd) const {
    __block NSString* ns_text = nil;

    void (^readBlock)(void) = ^{
        // Controls and their AppKit views are owned by the main thread.
        auto it = m_controls.find(ctrl_hwnd);
        if (it == m_controls.end()) {
            VITREO_TRACE("[cocoa] get_control_text: control 0x%x not found\n", ctrl_hwnd);
            return;
        }
        NSView* view = (__bridge NSView*)it->second;
        if ([view isKindOfClass:[NSTextField class]]) {
            NSTextField* field = (NSTextField*)view;
            // While a field is being edited AppKit keeps the live text in the
            // window's field editor and only commits it to -stringValue when
            // editing ends (focus loss / Enter). WM_GETTEXT must observe every
            // keystroke: a guest that validates or reformats on EN_CHANGE
            // otherwise reads the stale value and writes it straight back, so
            // the caret blinks in a field that never accepts a character.
            NSWindow* fwin = [field window];
            NSResponder* fr = fwin ? [fwin firstResponder] : nil;
            if ([fr isKindOfClass:[NSTextView class]] &&
                [(NSTextView*)fr delegate] == (id<NSTextViewDelegate>)field) {
                ns_text = [(NSTextView*)fr string];
            } else {
                ns_text = [field stringValue];
            }
        } else if (NSTextView* tv = vtr_document_text_view(view)) {
            ns_text = [tv string];
        } else if ([view isKindOfClass:[NSButton class]]) {
            ns_text = [(NSButton*)view title];
        } else if ([view isKindOfClass:[NSBox class]]) {
            ns_text = [(NSBox*)view title];
        } else if ([view isKindOfClass:[NSPopUpButton class]]) {
            ns_text = [[(NSPopUpButton*)view selectedItem] title];
        }
        // An owned snapshot: the main thread's pool may drain the
        // autoreleased string before this thread reads it.
        ns_text = [ns_text copy];
    };

    if ([NSThread isMainThread]) {
        readBlock();
    } else {
        vtr_run_on_main_sync(readBlock);
    }

    if (diag::win_diag())
        fprintf(stderr, "[gettext] get_control_text hwnd=0x%x -> '%s'\n",
                ctrl_hwnd, ns_text ? [ns_text UTF8String] : "(nil)");

    std::wstring text = (ns_text && [ns_text length] != 0)
        ? from_nsstring(ns_text) : std::wstring{};
    [ns_text release];
    return text;
}

// =============================================================================
// Custom VFS Folder Picker (Tree View)
// =============================================================================

std::string CocoaBridge::show_vfs_folder_picker(
        const std::string& initial_path,
        const std::string& title,
        VfsListCallback list_callback,
        VfsCreateDirCallback create_dir_callback) {
    ensure_app_initialized();

    __block std::string result;

    VITREO_TRACE( "[cocoa] show_vfs_folder_picker: title='%s'\n", title.c_str());

    void (^showPicker)(void) = ^{
        // Create the window
        NSRect windowRect = NSMakeRect(0, 0, 400, 450);
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:windowRect
                      styleMask:(NSWindowStyleMaskTitled |
                                NSWindowStyleMaskClosable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [window setTitle:[NSString stringWithUTF8String:title.c_str()]];
        [window center];

        NSView* contentView = window.contentView;

        // Title label at top
        NSTextField* titleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 410, 360, 20)];
        titleLabel.stringValue = @"Select a folder:";
        titleLabel.bordered = NO;
        titleLabel.editable = NO;
        titleLabel.backgroundColor = [NSColor clearColor];
        titleLabel.font = [NSFont boldSystemFontOfSize:13];
        [contentView addSubview:titleLabel];

        // Scroll view with outline view (tree)
        NSScrollView* scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 80, 360, 320)];
        scrollView.hasVerticalScroller = YES;
        scrollView.hasHorizontalScroller = NO;
        scrollView.autohidesScrollers = YES;
        scrollView.borderType = NSBezelBorder;

        NSOutlineView* outlineView = [[NSOutlineView alloc] initWithFrame:scrollView.bounds];
        outlineView.headerView = nil;
        outlineView.rowHeight = 22;
        outlineView.indentationPerLevel = 16;
        outlineView.indentationMarkerFollowsCell = YES;

        NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:@"Name"];
        column.width = 340;
        [outlineView addTableColumn:column];
        [outlineView setOutlineTableColumn:column];

        scrollView.documentView = outlineView;
        [contentView addSubview:scrollView];

        // Path display label — rounded bezel matches the macOS text field style
        NSTextField* pathLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 47, 360, 24)];
        pathLabel.stringValue = @"";
        pathLabel.editable = NO;
        pathLabel.selectable = YES;
        pathLabel.bezeled = YES;
        pathLabel.bezelStyle = NSTextFieldRoundedBezel;
        pathLabel.font = [NSFont systemFontOfSize:[NSFont systemFontSize]];
        [contentView addSubview:pathLabel];

        // Data source
        VtrFolderPickerDataSource* dataSource = [[VtrFolderPickerDataSource alloc] init];
        dataSource.pathLabel = pathLabel;
        dataSource.outlineView = outlineView;
        dataSource.listCallback = ^std::vector<vitreo::windowing::CocoaBridge::VfsDirEntry>(NSString* path) {
            return list_callback([path UTF8String]);
        };
        dataSource.createDirCallback = ^BOOL(NSString* path) {
            return create_dir_callback([path UTF8String]);
        };

        // Populate root nodes with mounted drives
        // Get drives from initial_path or default to C:
        NSString* initPath = [NSString stringWithUTF8String:initial_path.c_str()];
        if (initPath.length == 0) {
            initPath = @"C:\\";
        }

        // For now, just add the drives that the callback knows about
        // We'll ask for the root listing to discover drives
        auto root_entries = list_callback("");
        if (root_entries.empty()) {
            // Default to C: if no drives returned
            VtrFolderNode* cDrive = [[VtrFolderNode alloc] initWithName:@"C:"
                                                                  path:@"C:\\"
                                                            expandable:YES];
            [dataSource.rootNodes addObject:cDrive];
        } else {
            for (const auto& entry : root_entries) {
                NSString* name = [NSString stringWithUTF8String:entry.name.c_str()];
                NSString* path = [NSString stringWithFormat:@"%@\\", name];
                VtrFolderNode* node = [[VtrFolderNode alloc] initWithName:name
                                                                    path:path
                                                              expandable:YES];
                [dataSource.rootNodes addObject:node];
            }
        }

        outlineView.dataSource = dataSource;
        outlineView.delegate = dataSource;
        
        // Retain the data source by associating it with the window
        // (NSOutlineView only holds a weak reference to its dataSource)
        static char kDataSourceKey;
        objc_setAssociatedObject(window, &kDataSourceKey, dataSource,
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        
        [outlineView reloadData];

        // Expand first root node
        if (dataSource.rootNodes.count > 0) {
            [outlineView expandItem:dataSource.rootNodes[0]];
        }

        // Buttons
        NSButton* okButton = [[NSButton alloc] initWithFrame:NSMakeRect(290, 10, 90, 30)];
        okButton.title = @"OK";
        okButton.bezelStyle = NSBezelStyleRounded;
        okButton.keyEquivalent = @"\r";  // Enter key
        okButton.target = dataSource;
        okButton.action = @selector(okClicked:);
        [contentView addSubview:okButton];

        NSButton* cancelButton = [[NSButton alloc] initWithFrame:NSMakeRect(190, 10, 90, 30)];
        cancelButton.title = @"Cancel";
        cancelButton.bezelStyle = NSBezelStyleRounded;
        cancelButton.keyEquivalent = @"\033";  // Escape key
        cancelButton.target = dataSource;
        cancelButton.action = @selector(cancelClicked:);
        [contentView addSubview:cancelButton];

        NSButton* newFolderButton = [[NSButton alloc] initWithFrame:NSMakeRect(20, 10, 130, 30)];
        newFolderButton.title = @"Make New Folder";
        newFolderButton.bezelStyle = NSBezelStyleRounded;
        newFolderButton.target = dataSource;
        newFolderButton.action = @selector(makeNewFolderClicked:);
        [contentView addSubview:newFolderButton];

        // Run modal
        [NSApp activateIgnoringOtherApps:YES];
        NSModalResponse response = [NSApp runModalForWindow:window];

        if (response == NSModalResponseOK && dataSource.selectedPath.length > 0) {
            result = [dataSource.selectedPath UTF8String];
        }

        [window close];
    };

    if ([NSThread isMainThread]) {
        showPicker();
    } else {
        dispatch_sync(dispatch_get_main_queue(), showPicker);
    }

    VITREO_TRACE( "[cocoa] show_vfs_folder_picker: result='%s'\n", result.c_str());
    return result;
}

} // namespace vitreo::windowing
