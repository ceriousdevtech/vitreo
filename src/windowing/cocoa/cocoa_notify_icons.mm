/// @file cocoa_notify_icons.mm
/// @brief Notification-area icons (Shell_NotifyIcon) as menu-bar status items.
///
/// .docs/windowing-native-presentation.md §4: the Windows notification area
/// is the macOS menu bar. shell32 (notify_icons.cpp) keeps the Win32
/// contract and turns the input reported here into the callback messages a
/// real click delivers (tools/oracle_probe/notifyicon_click_probe).
///
/// This file is built without ARC (as the rest of src/windowing/cocoa): the
/// bridge owns one +1 reference to each VtrStatusItemHost in m_notify_icons.
///
/// A click on the item activates the app first. Explorer lets an application
/// take the foreground from a click on its icon (that is how its
/// SetForegroundWindow + TrackPopupMenu or ShowWindow works); on macOS the
/// equivalent is the app becoming active.

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/cocoa/cocoa_bridge_internal.h"
#include "windowing/cocoa/objc_helpers.h"

using vitreo::u32;
using vitreo::u64;
using vitreo::u8;
using vitreo::windowing::CocoaBridge;
using vitreo::windowing::NotifyIconBalloon;
using vitreo::windowing::NotifyIconInput;
using vitreo::windowing::NotifyIconPresentation;

namespace {

/// Menu-bar icons are 18 points square.
constexpr CGFloat kStatusIconPoints = 18.0;
/// Windows 11 dismisses a notification after the accessibility "Dismiss
/// notifications after" time, 5 seconds by default (Learn, uTimeout: display
/// times follow the system accessibility settings since Vista).
constexpr NSTimeInterval kBalloonSeconds = 5.0;

NSString* utf8(const std::string& text) {
    return [[NSString alloc] initWithBytes:text.data()
                                    length:text.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

NSImage* image_from_rgba(const std::vector<u8>& rgba, u32 width, u32 height, CGFloat points) {
    if (rgba.empty() || width == 0 || height == 0 || rgba.size() < static_cast<size_t>(width) * height * 4) {
        return [[[NSImage alloc] initWithSize:NSMakeSize(points, points)] autorelease];
    }
    NSBitmapImageRep* rep = [[[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nullptr
                      pixelsWide:width
                      pixelsHigh:height
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                    bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                     bytesPerRow:width * 4
                    bitsPerPixel:32] autorelease];
    std::memcpy([rep bitmapData], rgba.data(), static_cast<size_t>(width) * height * 4);
    // GDI colours are sRGB.
    rep = [rep bitmapImageRepByRetaggingWithColorSpace:[NSColorSpace sRGBColorSpace]];
    NSImage* image = [[[NSImage alloc] initWithSize:NSMakeSize(points, points)] autorelease];
    [image addRepresentation:rep];
    return image;
}

}  // namespace

@class VtrBalloonPanel;

@interface VtrStatusItemHost : NSObject
@property(nonatomic, retain) NSStatusItem* item;
@property(nonatomic, assign) CocoaBridge* bridge;
@property(nonatomic, assign) u64 key;
@property(nonatomic, retain) NSTrackingArea* tracking;
@property(nonatomic, retain) NSPanel* balloon;
/// The run loop retains the timer (and the timer this host); invalidated on close.
@property(nonatomic, assign) NSTimer* balloonTimer;
- (void)closeBalloonReporting:(BOOL)report clicked:(BOOL)clicked;
@end

@interface VtrBalloonView : NSView
@property(nonatomic, assign) VtrStatusItemHost* host;
@end

@implementation VtrBalloonView
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}
- (void)mouseDown:(NSEvent*)event {
    (void)event;
    [self.host closeBalloonReporting:YES clicked:YES];
}
@end

@implementation VtrStatusItemHost
- (void)dealloc {
    [_item release];
    [_tracking release];
    [_balloon release];
    [super dealloc];
}
- (void)clicked:(id)sender {
    (void)sender;
    NSEvent* event = [NSApp currentEvent];
    const bool right = event.type == NSEventTypeRightMouseUp ||
                       (event.modifierFlags & NSEventModifierFlagControl) != 0;
    [NSApp activateIgnoringOtherApps:YES];
    if (!self.bridge) return;
    // The action fires on mouse up; the press is reported with it, so the
    // owner sees the measured down/up pair in order.
    if (right) {
        self.bridge->report_notify_icon_input(self.key, NotifyIconInput::RightDown);
        self.bridge->report_notify_icon_input(self.key, NotifyIconInput::RightUp);
    } else {
        self.bridge->report_notify_icon_input(
            self.key, event.clickCount >= 2 ? NotifyIconInput::LeftDoubleDown : NotifyIconInput::LeftDown);
        self.bridge->report_notify_icon_input(self.key, NotifyIconInput::LeftUp);
    }
}
- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    if (self.bridge) self.bridge->report_notify_icon_input(self.key, NotifyIconInput::HoverEnter);
}
- (void)mouseExited:(NSEvent*)event {
    (void)event;
    if (self.bridge) self.bridge->report_notify_icon_input(self.key, NotifyIconInput::HoverExit);
}
- (void)balloonTimedOut:(NSTimer*)timer {
    (void)timer;
    [self closeBalloonReporting:YES clicked:NO];
}
- (void)closeBalloonReporting:(BOOL)report clicked:(BOOL)clicked {
    [self.balloonTimer invalidate];
    self.balloonTimer = nil;
    if (self.balloon == nil) return;
    [self.balloon orderOut:nil];
    self.balloon = nil;
    if (report && self.bridge) {
        self.bridge->report_notify_icon_input(
            self.key, clicked ? NotifyIconInput::BalloonClicked : NotifyIconInput::BalloonTimedOut);
    }
}
@end

namespace vitreo::windowing {

void CocoaBridge::set_notify_icon_input_handler(NotifyIconInputHandler handler) {
    m_notify_icon_input = std::move(handler);
}

void CocoaBridge::set_notify_icon_reopen_target(std::function<std::optional<u64>()> target) {
    m_notify_icon_reopen_target = std::move(target);
}

void CocoaBridge::present_notify_icon(u64 key, const NotifyIconPresentation& view_in) {
    ensure_app_initialized();
    const NotifyIconPresentation view = view_in;
    vtr_run_on_main_async(^{
        @autoreleasepool {
            VtrStatusItemHost* host = nil;
            auto found = m_notify_icons.find(key);
            if (found == m_notify_icons.end()) {
                host = [[VtrStatusItemHost alloc] init];
                host.bridge = this;
                host.key = key;
                host.item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
                NSStatusBarButton* button = host.item.button;
                button.target = host;
                button.action = @selector(clicked:);
                [button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
                host.tracking = [[[NSTrackingArea alloc]
                    initWithRect:NSZeroRect
                         options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                                 NSTrackingInVisibleRect
                           owner:host
                        userInfo:nil] autorelease];
                [button addTrackingArea:host.tracking];
                m_notify_icons[key] = (__bridge void*)host;  // the alloc's +1
            } else {
                host = (__bridge VtrStatusItemHost*)found->second;
            }
            NSStatusBarButton* button = host.item.button;
            button.image = image_from_rgba(view.rgba, view.width, view.height, kStatusIconPoints);
            button.imageScaling = NSImageScaleProportionallyUpOrDown;
            button.toolTip = view.native_tooltip && !view.tooltip.empty()
                                 ? utf8(view.tooltip) : nil;
            host.item.visible = !view.hidden;
        }
    });
}

void CocoaBridge::withdraw_notify_icon(u64 key) {
    vtr_run_on_main_async(^{
        auto found = m_notify_icons.find(key);
        if (found == m_notify_icons.end()) return;
        VtrStatusItemHost* host = (__bridge VtrStatusItemHost*)found->second;
        m_notify_icons.erase(found);
        [host closeBalloonReporting:NO clicked:NO];
        host.bridge = nullptr;
        [host.item.button removeTrackingArea:host.tracking];
        [[NSStatusBar systemStatusBar] removeStatusItem:host.item];
        [host release];
    });
}

std::optional<Rect> CocoaBridge::notify_icon_frame(u64 key) {
    __block std::optional<Rect> frame;
    vtr_run_on_main_sync(^{
        auto found = m_notify_icons.find(key);
        if (found == m_notify_icons.end()) return;
        VtrStatusItemHost* host = (__bridge VtrStatusItemHost*)found->second;
        NSWindow* window = host.item.button.window;
        if (!host.item.visible || window == nil) return;
        const NSRect r = window.frame;
        // The menu bar places a new item a run-loop turn after it is made;
        // until then it is not on screen (GetRect: S_FALSE, as for hidden).
        if (r.size.width <= 0 || r.size.height <= 0) return;
        const CGFloat top_y = cocoa_virtual_top_y();
        frame = Rect{static_cast<i32>(r.origin.x),
                     static_cast<i32>(top_y - (r.origin.y + r.size.height)),
                     static_cast<i32>(r.origin.x + r.size.width),
                     static_cast<i32>(top_y - r.origin.y)};
    });
    return frame;
}

void CocoaBridge::show_notify_icon_balloon(u64 key, const NotifyIconBalloon& balloon_in) {
    const NotifyIconBalloon balloon = balloon_in;
    vtr_run_on_main_async(^{
        @autoreleasepool {
            auto found = m_notify_icons.find(key);
            if (found == m_notify_icons.end()) return;
            VtrStatusItemHost* host = (__bridge VtrStatusItemHost*)found->second;
            [host closeBalloonReporting:NO clicked:NO];

            NSStackView* text = [NSStackView stackViewWithViews:@[]];
            text.orientation = NSUserInterfaceLayoutOrientationVertical;
            text.alignment = NSLayoutAttributeLeading;
            text.spacing = 2;
            if (!balloon.title.empty()) {
                NSTextField* title = [NSTextField labelWithString:utf8(balloon.title)];
                title.font = [NSFont boldSystemFontOfSize:13];
                [text addArrangedSubview:title];
            }
            NSTextField* body = [NSTextField wrappingLabelWithString:utf8(balloon.text)];
            body.preferredMaxLayoutWidth = 280;
            [text addArrangedSubview:body];

            NSImage* icon = nil;
            switch (balloon.kind) {
            case NotifyIconBalloon::Kind::Info: icon = [NSImage imageNamed:NSImageNameInfo]; break;
            case NotifyIconBalloon::Kind::Warning: icon = [NSImage imageNamed:NSImageNameCaution]; break;
            case NotifyIconBalloon::Kind::Error:
                icon = [NSImage imageWithSystemSymbolName:@"xmark.octagon.fill"
                                 accessibilityDescription:nil];
                break;
            case NotifyIconBalloon::Kind::User:
                icon = image_from_rgba(balloon.user_rgba, balloon.user_width, balloon.user_height,
                                       balloon.large_icon ? 32.0 : 16.0);
                break;
            case NotifyIconBalloon::Kind::None: break;
            }
            NSStackView* row = [NSStackView stackViewWithViews:@[]];
            row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            row.alignment = NSLayoutAttributeTop;
            row.spacing = 8;
            row.edgeInsets = NSEdgeInsetsMake(10, 12, 10, 12);
            if (icon != nil) {
                const CGFloat side = balloon.large_icon ? 32.0 : 16.0;
                NSImageView* iv = [NSImageView imageViewWithImage:icon];
                [iv.widthAnchor constraintEqualToConstant:side].active = YES;
                [iv.heightAnchor constraintEqualToConstant:side].active = YES;
                [row addArrangedSubview:iv];
            }
            [row addArrangedSubview:text];

            VtrBalloonView* content = [[[VtrBalloonView alloc] initWithFrame:NSZeroRect] autorelease];
            content.host = host;
            content.wantsLayer = YES;
            content.layer.cornerRadius = 10;
            content.layer.backgroundColor = [NSColor windowBackgroundColor].CGColor;
            row.translatesAutoresizingMaskIntoConstraints = NO;
            [content addSubview:row];
            [row.leadingAnchor constraintEqualToAnchor:content.leadingAnchor].active = YES;
            [row.trailingAnchor constraintEqualToAnchor:content.trailingAnchor].active = YES;
            [row.topAnchor constraintEqualToAnchor:content.topAnchor].active = YES;
            [row.bottomAnchor constraintEqualToAnchor:content.bottomAnchor].active = YES;
            const NSSize size = row.fittingSize;

            NSPanel* panel = [[[NSPanel alloc]
                initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                          styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                            backing:NSBackingStoreBuffered
                              defer:NO] autorelease];
            panel.level = NSStatusWindowLevel;
            panel.opaque = NO;
            panel.backgroundColor = [NSColor clearColor];
            panel.hasShadow = YES;
            panel.releasedWhenClosed = NO;
            panel.contentView = content;
            // Under the status item; at the top-right of the screen when the
            // item is hidden (Windows shows a notification for a hidden icon
            // too, from the corner of the screen).
            NSWindow* anchor = host.item.button.window;
            NSScreen* screen = anchor.screen ?: [NSScreen mainScreen];
            NSPoint origin;
            if (host.item.visible && anchor != nil) {
                const NSRect a = anchor.frame;
                origin = NSMakePoint(a.origin.x + a.size.width / 2 - size.width / 2,
                                     a.origin.y - size.height - 4);
            } else {
                const NSRect v = screen.visibleFrame;
                origin = NSMakePoint(NSMaxX(v) - size.width - 12, NSMaxY(v) - size.height - 12);
            }
            const NSRect visible = screen.visibleFrame;
            origin.x = std::max(NSMinX(visible) + 4, std::min(origin.x, NSMaxX(visible) - size.width - 4));
            [panel setFrameOrigin:origin];
            [panel orderFrontRegardless];
            host.balloon = panel;
            host.balloonTimer = [NSTimer scheduledTimerWithTimeInterval:kBalloonSeconds
                                                                  target:host
                                                                selector:@selector(balloonTimedOut:)
                                                                userInfo:nil
                                                                 repeats:NO];
            if (balloon.sound) NSBeep();
            report_notify_icon_input(key, NotifyIconInput::BalloonShown);
        }
    });
}

void CocoaBridge::hide_notify_icon_balloon(u64 key) {
    vtr_run_on_main_async(^{
        auto found = m_notify_icons.find(key);
        if (found == m_notify_icons.end()) return;
        VtrStatusItemHost* host = (__bridge VtrStatusItemHost*)found->second;
        [host closeBalloonReporting:NO clicked:NO];
    });
}

void CocoaBridge::report_notify_icon_input(u64 key, NotifyIconInput input) {
    if (!m_notify_icon_input) return;
    const NSPoint loc = [NSEvent mouseLocation];
    const CGFloat top_y = cocoa_virtual_top_y();
    m_notify_icon_input(key, input, static_cast<i32>(loc.x), static_cast<i32>(top_y - loc.y));
}

void CocoaBridge::on_dock_reopen(bool has_visible_windows) {
    if (route_dock_session()) return;
    // A minimized window comes back through SC_RESTORE
    // (on_app_did_become_active); a window the app hid in its tray icon has
    // no Dock tile, so the Dock click goes to that icon instead.
    if (has_visible_windows || !m_notify_icon_reopen_target) return;
    for (const auto& [hwnd, state] : m_windows) {
        (void)hwnd;
        if (state.ns_window == nullptr) continue;
        NSWindow* win = reinterpret_cast<NSWindow*>(state.ns_window);
        if ([win isMiniaturized] || [win isVisible]) return;
    }
    if (const auto key = m_notify_icon_reopen_target()) {
        report_notify_icon_input(*key, NotifyIconInput::Activate);
    }
}

}  // namespace vitreo::windowing
