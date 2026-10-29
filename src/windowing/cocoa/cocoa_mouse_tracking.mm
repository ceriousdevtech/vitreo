// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_mouse_tracking.mm
/// @brief TrackMouseEvent delivery and the comctl32 tooltip bubble.
///
/// Both exist because the tooltip path needs them.
/// The guest creates a real `tooltips_class32` control and drives it with
/// TTM_* messages, and arms TrackMouseEvent(TME_LEAVE) to learn when the
/// pointer leaves. With TrackMouseEvent refusing and the tooltip class having
/// no control behind it, the guest showed an unpainted white popup that never
/// went away.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/messages/message_queue.hpp"
#include "windowing/messages/message_types.hpp"

#include <algorithm>
#include <string>

namespace vitreo::windowing {

namespace {

/// Win32's own default when TRACKMOUSEEVENT asks for HOVER_DEFAULT.
constexpr u32 kDefaultHoverMs = 400;
/// Windows lets the pointer wander inside SM_CXDOUBLECLK/SM_CYDOUBLECLK
/// without restarting the hover dwell.
constexpr i32 kHoverSlop = 4;

NSString* to_ns(const std::wstring& text) {
    std::u16string utf16;
    utf16.reserve(text.size());
    for (const wchar_t ch : text) {
        const char32_t c = static_cast<char32_t>(ch);
        if (c > 0xFFFF) {
            const char32_t v = c - 0x10000;
            utf16.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            utf16.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        } else {
            utf16.push_back(static_cast<char16_t>(c));
        }
    }
    NSString* s = [[NSString alloc]
        initWithBytes:utf16.data()
               length:utf16.size() * sizeof(char16_t)
             encoding:NSUTF16LittleEndianStringEncoding];
    return s != nil ? s : @"";
}

NSFont* tooltip_font() { return [NSFont toolTipsFontOfSize:0.0]; }

NSSize measure(NSString* text, i32 max_width) {
    NSMutableParagraphStyle* para = [[NSMutableParagraphStyle alloc] init];
    [para setLineBreakMode:NSLineBreakByWordWrapping];
    NSDictionary* attrs = @{NSFontAttributeName : tooltip_font(),
                            NSParagraphStyleAttributeName : para};
    const CGFloat bound = max_width > 0 ? static_cast<CGFloat>(max_width)
                                        : 640.0;
    NSRect r = [text boundingRectWithSize:NSMakeSize(bound, 10000.0)
                                  options:NSStringDrawingUsesLineFragmentOrigin
                               attributes:attrs];
    return NSMakeSize(std::ceil(r.size.width), std::ceil(r.size.height));
}

} // namespace

// =============================================================================
// TrackMouseEvent
// =============================================================================

void CocoaBridge::set_mouse_capture(HWND hwnd) {
    m_mouse_capture_hwnd.store(hwnd, std::memory_order_release);
}

void CocoaBridge::track_mouse_event(HWND hwnd, bool leave, bool hover,
                                    u32 hover_ms) {
    if (hwnd == NULL_HWND) return;
    MouseTracking& t = m_mouse_tracking[hwnd];
    if (leave) t.leave = true;
    if (hover) {
        t.hover = true;
        t.hover_ms = hover_ms == 0 || hover_ms == 0xFFFFFFFFu ? kDefaultHoverMs
                                                             : hover_ms;
        // The dwell is measured from now, at wherever the pointer already is.
        t.origin_x = m_last_mouse_x;
        t.origin_y = m_last_mouse_y;
        t.hover_deadline = CACurrentMediaTime() +
                           static_cast<double>(t.hover_ms) / 1000.0;
    }
}

void CocoaBridge::cancel_mouse_tracking(HWND hwnd, bool leave, bool hover) {
    auto it = m_mouse_tracking.find(hwnd);
    if (it == m_mouse_tracking.end()) return;
    if (leave) it->second.leave = false;
    if (hover) it->second.hover = false;
    if (!it->second.leave && !it->second.hover) m_mouse_tracking.erase(it);
}

void CocoaBridge::query_mouse_tracking(HWND hwnd, bool& leave, bool& hover,
                                       u32& hover_ms) const {
    leave = false;
    hover = false;
    hover_ms = 0;
    const auto it = m_mouse_tracking.find(hwnd);
    if (it == m_mouse_tracking.end()) return;
    leave = it->second.leave;
    hover = it->second.hover;
    hover_ms = it->second.hover_ms;
}

void CocoaBridge::flush_mouse_tracking(MessageQueue& queue, HWND over_hwnd,
                                       i32 client_x, i32 client_y) {
    if (m_mouse_tracking.empty()) {
        m_mouse_last_hwnd = over_hwnd;
        return;
    }
    const double now = CACurrentMediaTime();
    std::vector<HWND> retired;
    for (auto& [hwnd, track] : m_mouse_tracking) {
        if (track.leave && hwnd != over_hwnd) {
            // Learn: WM_MOUSELEAVE carries no coordinates, and the request is
            // cancelled by its own delivery.
            MSG leave_msg{};
            leave_msg.hwnd = hwnd;
            leave_msg.message = WM::MOUSELEAVE;
            (void)queue.post(leave_msg);
            track.leave = false;
        }
        if (track.hover && hwnd == over_hwnd) {
            const bool moved =
                std::abs(client_x - track.origin_x) > kHoverSlop ||
                std::abs(client_y - track.origin_y) > kHoverSlop;
            if (moved) {
                track.origin_x = client_x;
                track.origin_y = client_y;
                track.hover_deadline =
                    now + static_cast<double>(track.hover_ms) / 1000.0;
            } else if (now >= track.hover_deadline) {
                MSG hover_msg{};
                hover_msg.hwnd = hwnd;
                hover_msg.message = WM::MOUSEHOVER;
                hover_msg.lParam =
                    (static_cast<u64>(static_cast<u16>(client_y)) << 16) |
                    static_cast<u16>(client_x);
                (void)queue.post(hover_msg);
                track.hover = false;
            }
        }
        if (!track.leave && !track.hover) retired.push_back(hwnd);
    }
    for (const HWND hwnd : retired) m_mouse_tracking.erase(hwnd);
    m_mouse_last_hwnd = over_hwnd;
}

// =============================================================================
// Tooltip bubble
// =============================================================================

void CocoaBridge::measure_tooltip(const std::wstring& text, i32 max_width,
                                  i32& out_width, i32& out_height) const {
    __block NSSize size = NSMakeSize(0.0, 0.0);
    NSString* ns = to_ns(text);
    vtr_run_on_main_sync(^{
        @autoreleasepool { size = measure(ns, max_width); }
    });
    // The bubble is the text plus Windows' tooltip margins.
    out_width = static_cast<i32>(size.width) + 12;
    out_height = static_cast<i32>(size.height) + 6;
}

void CocoaBridge::show_tooltip(HWND hwnd, const std::wstring& text,
                               i32 screen_x, i32 screen_y) {
    if (text.empty()) {
        hide_tooltip(hwnd);
        return;
    }
    NSString* ns = to_ns(text);
    const i32 x = screen_x;
    const i32 y = screen_y;
    vtr_run_on_main_sync(^{
        @autoreleasepool {
            NSPanel* panel = static_cast<NSPanel*>(m_tooltip_panels[hwnd]);
            NSTextField* label = nil;
            if (panel == nil) {
                panel = [[NSPanel alloc]
                    initWithContentRect:NSMakeRect(0, 0, 10, 10)
                              styleMask:NSWindowStyleMaskBorderless
                                backing:NSBackingStoreBuffered
                                  defer:NO];
                [panel setOpaque:NO];
                [panel setHasShadow:YES];
                [panel setLevel:NSPopUpMenuWindowLevel];
                [panel setBackgroundColor:[NSColor clearColor]];
                // A tooltip must never take focus or steal the pointer.
                [panel setIgnoresMouseEvents:YES];
                [panel setBecomesKeyOnlyIfNeeded:YES];
                [panel setHidesOnDeactivate:NO];
                [panel setCollectionBehavior:
                    NSWindowCollectionBehaviorCanJoinAllSpaces |
                    NSWindowCollectionBehaviorTransient |
                    NSWindowCollectionBehaviorIgnoresCycle];

                NSVisualEffectView* back = [[NSVisualEffectView alloc]
                    initWithFrame:NSMakeRect(0, 0, 10, 10)];
                [back setMaterial:NSVisualEffectMaterialToolTip];
                [back setBlendingMode:NSVisualEffectBlendingModeBehindWindow];
                [back setState:NSVisualEffectStateActive];
                [back setWantsLayer:YES];
                [[back layer] setCornerRadius:5.0];
                [[back layer] setMasksToBounds:YES];

                label = [[NSTextField alloc] initWithFrame:NSZeroRect];
                [label setBezeled:NO];
                [label setDrawsBackground:NO];
                [label setEditable:NO];
                [label setSelectable:NO];
                [label setFont:tooltip_font()];
                [label setTextColor:[NSColor textColor]];
                [label setLineBreakMode:NSLineBreakByWordWrapping];
                [label setIdentifier:@"VtrTooltipLabel"];
                [back addSubview:label];
                [panel setContentView:back];
                // Non-ARC target: the map owns one retain.
                [panel retain];
                m_tooltip_panels[hwnd] = panel;
            } else {
                for (NSView* v in [[panel contentView] subviews]) {
                    if ([[v identifier] isEqualToString:@"VtrTooltipLabel"]) {
                        label = (NSTextField*)v;
                        break;
                    }
                }
            }
            if (label == nil) return;
            [label setStringValue:ns];
            const NSSize text_size = measure(ns, 600);
            const NSSize bubble = NSMakeSize(text_size.width + 12.0,
                                             text_size.height + 6.0);
            [label setFrame:NSMakeRect(6.0, 3.0, text_size.width,
                                       text_size.height)];
            [[panel contentView] setFrame:NSMakeRect(0, 0, bubble.width,
                                                     bubble.height)];

            // TTM_TRACKPOSITION gives a Win32 screen point (y down from the
            // primary display's top-left); AppKit wants y up from its
            // bottom-left, and the panel is positioned by its own bottom-left.
            const CGFloat screen_h = [[NSScreen screens] count] > 0
                ? NSMaxY([[[NSScreen screens] objectAtIndex:0] frame])
                : 0.0;
            const NSRect frame = NSMakeRect(
                static_cast<CGFloat>(x),
                screen_h - static_cast<CGFloat>(y) - bubble.height,
                bubble.width, bubble.height);
            [panel setFrame:frame display:YES];
            [panel orderFront:nil];
        }
    });
}

void CocoaBridge::hide_tooltip(HWND hwnd) {
    const auto it = m_tooltip_panels.find(hwnd);
    if (it == m_tooltip_panels.end()) return;
    void* raw = it->second;
    m_tooltip_panels.erase(it);
    vtr_run_on_main_sync(^{
        @autoreleasepool {
            NSPanel* panel = static_cast<NSPanel*>(raw);
            [panel orderOut:nil];
            [panel release];
        }
    });
}

} // namespace vitreo::windowing
