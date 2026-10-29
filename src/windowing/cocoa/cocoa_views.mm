// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_views.mm
/// @brief The AppKit window and view classes behind an HWND.
///
/// VtrWindow and VtrPanel are the NSWindow subclasses a top-level HWND lives
/// in. VtrContentView is its permanent flipped content view, holding child
/// controls in Win32 Y-down coordinates; VtrPixelView carries GDI pixels,
/// VtrMetalView Metal presentation, and VtrContainerView embedded child
/// dialogs.
///
/// Split out of cocoa_bridge.mm, which was 7,624 lines. The classes are
/// unchanged; only their translation unit is new.

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

// =============================================================================
// VtrPixelView — layer-backed view for fast framebuffer blitting via CGImage
// =============================================================================


@implementation VtrPixelView

// Guest client pixels never move the window. NSView's default answers YES
// for a non-opaque view, which lets AppKit drag the window from its
// "background"; Win32 moves a window only for WM_NCHITTEST HTCAPTION
// (perform_guest_frame_mouse_down / begin_native_window_drag).
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        // Layer-HOSTING view: we own the layer so AppKit never overwrites
        // layer.contents during live resize or other view invalidation.
        CALayer* hostingLayer = [CALayer layer];
        // Do not flip this view or its layer. VtrContentView is flipped so
        // child controls use Win32 Y-down frames; this view is a layer-hosting
        // GDI plate. CALayer.contents is drawn with the image top at the
        // visual top only when the hosting view is unflipped. isFlipped and
        // geometryFlipped both left installer framebuffers upside down.
        hostingLayer.geometryFlipped = NO;
        hostingLayer.contentsGravity = kCAGravityTopLeft;
        hostingLayer.magnificationFilter = kCAFilterNearest;
        // Clear, not black: a covering skin host shown before the page
        // GDI flush was a solid black rectangle on Next/Back.
        hostingLayer.backgroundColor = NSColor.clearColor.CGColor;
        // Disable implicit animations for instant content updates
        hostingLayer.actions = @{@"contents": [NSNull null]};
        self.layer = hostingLayer;   // must set layer BEFORE wantsLayer
        self.wantsLayer = YES;
        self.scalesFramebuffer = NO;
        self.passesThroughEvents = NO;
        self.framebufferWidth = 0;
        self.framebufferHeight = 0;
    }
    return self;
}

- (BOOL)isFlipped { return NO; }
- (NSView*)hitTest:(NSPoint)point {
    return self.passesThroughEvents ? nil : [super hitTest:point];
}
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event { (void)event; }
- (void)keyUp:(NSEvent*)event { (void)event; }
- (void)flagsChanged:(NSEvent*)event { (void)event; }
// Guest-painted pixels take their mouse input as WM_* messages, which the
// event pump posts after [NSApp sendEvent:]. Passing a click up the responder
// chain let a native view underneath (a list's NSTableView) start a tracking
// loop that consumed the mouse-up, so the guest saw downs without ups.
- (void)mouseDown:(NSEvent*)event { (void)event; }
- (void)mouseUp:(NSEvent*)event { (void)event; }
- (void)mouseDragged:(NSEvent*)event { (void)event; }
- (void)rightMouseDown:(NSEvent*)event { (void)event; }
- (void)rightMouseUp:(NSEvent*)event { (void)event; }
- (void)rightMouseDragged:(NSEvent*)event { (void)event; }
- (void)otherMouseDown:(NSEvent*)event { (void)event; }
- (void)otherMouseUp:(NSEvent*)event { (void)event; }
- (void)otherMouseDragged:(NSEvent*)event { (void)event; }

- (void)resetCursorRects {
    if (!vtr_view_reset_cursor_rects(self)) {
        [super resetCursorRects];
    }
}

- (void)cursorUpdate:(NSEvent*)event {
    if (!vtr_view_cursor_update(self)) {
        [super cursorUpdate:event];
    }
}

- (void)updatePixels:(const uint8_t*)rgba width:(int32_t)w height:(int32_t)h {
    if (!rgba || w <= 0 || h <= 0) return;
    self.framebufferWidth = w;
    self.framebufferHeight = h;

    // SoftBitmap is top-down RGBA. CGBitmapContextCreate is bottom-up, so
    // wrapping that buffer and calling CreateImage inverted every GDI flush
    // (installer wizard pages, splash bitmaps). CGImage's first row is top.
    const size_t bytesPerRow = static_cast<size_t>(w) * 4;
    const size_t nbytes = bytesPerRow * static_cast<size_t>(h);
    NSData* copy = [NSData dataWithBytes:rgba length:nbytes];
    CGDataProviderRef provider =
        CGDataProviderCreateWithCFData((__bridge CFDataRef)copy);
    if (!provider) return;

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // GDI SoftBitmaps are RGBA in memory but typically leave A=0. Treating
    // that as premultiplied-last (a wizard's clear host plates) makes the
    // window-server hit mask empty, so clicks activate the HWND behind the
    // guest. Non-layered Win32 painting is opaque over the window rect.
    // A child window's surface (compositesAlpha) sits inside that opaque
    // window, and keeps A=0 only where the child never painted.
    const BOOL alpha = self.compositesAlpha;
    CGImageRef img = CGImageCreate(
        static_cast<size_t>(w), static_cast<size_t>(h), 8, 32, bytesPerRow, cs,
        (alpha ? kCGImageAlphaPremultipliedLast : kCGImageAlphaNoneSkipLast) |
            kCGBitmapByteOrderDefault,
        provider, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);
    if (!img) return;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self.layer.contents = (__bridge id)img;
    self.layer.opaque = !alpha;
    [CATransaction commit];
    [CATransaction flush];

    CGImageRelease(img);
}

@end

// =============================================================================
// VtrContentView — flipped NSView used as the permanent contentView so that
// child controls (buttons, text fields) are in Win32 Y-down coordinates and
// are never destroyed when VtrPixelView is added later.
// =============================================================================

@implementation VtrContentView
- (BOOL)isFlipped { return YES; }
// Guest client pixels never move the window. NSView's default answers YES
// for a non-opaque view, which lets AppKit drag the window from its
// "background"; Win32 moves a window only for WM_NCHITTEST HTCAPTION
// (perform_guest_frame_mouse_down / begin_native_window_drag).
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (NSView*)hitTest:(NSPoint)point {
    NSView* hit = [super hitTest:point];
    if (hit != nil) {
        return hit;
    }
    // Pixel views and containers may pass events through to siblings. A miss
    // still belongs to this HWND unless the window is WS_EX_TRANSPARENT
    // (ignoresMouseEvents), matching Win32's window-rect hit test.
    const NSPoint local = [self convertPoint:point fromView:self.superview];
    return NSPointInRect(local, self.bounds) ? self : nil;
}

// Vitreo translates key events into Win32 messages itself while draining the
// NSApp queue, but every event is also sent through the responder chain. With
// no keyDown:/keyUp: handler AppKit treats each keystroke as unhandled and
// rings the system alert, so the view accepts key events and consumes them.
// Native child controls remain first responder when focused and keep their own
// key handling.
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event { (void)event; }
- (void)keyUp:(NSEvent*)event { (void)event; }
- (void)flagsChanged:(NSEvent*)event { (void)event; }

// When this content view is first responder (common after dialog show), Cmd+V
// / Edit ▸ Paste land here. Forward clipboard actions to a native EDIT so
// a wizard's text and path fields accept paste.
- (NSTextField*)vtr_findEditableTextField {
    NSMutableArray<NSView*>* stack = [NSMutableArray arrayWithObject:self];
    NSTextField* fallback = nil;
    while (stack.count > 0) {
        NSView* v = stack.lastObject;
        [stack removeLastObject];
        if ([v isKindOfClass:[NSTextField class]]) {
            NSTextField* tf = (NSTextField*)v;
            if ([tf isEditable] && ![tf isHiddenOrHasHiddenAncestor] && [tf currentEditor] != nil) {
                return tf;
            }
            if ([tf isEditable] && ![tf isHiddenOrHasHiddenAncestor] && fallback == nil) {
                fallback = tf;
            }
        }
        for (NSView* child in [v subviews]) {
            [stack addObject:child];
        }
    }
    return fallback;
}

- (id)vtr_textEditingTarget {
    NSWindow* win = [self window];
    NSResponder* resp = win ? [win firstResponder] : nil;
    if ([resp isKindOfClass:[NSTextView class]] ||
        [resp isKindOfClass:[NSText class]]) {
        return resp;
    }
    if ([resp isKindOfClass:[NSTextField class]]) {
        NSText* editor = [win fieldEditor:YES forObject:(NSTextField*)resp];
        return editor ?: resp;
    }
    NSTextField* field = [self vtr_findEditableTextField];
    if (!field || !win) return nil;
    [win makeFirstResponder:field];
    NSText* editor = [win fieldEditor:YES forObject:field];
    return editor ?: field;
}

- (BOOL)respondsToSelector:(SEL)sel {
    if (sel == @selector(paste:) || sel == @selector(copy:) ||
        sel == @selector(cut:) || sel == @selector(selectAll:)) {
        return YES;
    }
    return [super respondsToSelector:sel];
}

- (void)paste:(id)sender {
    id target = [self vtr_textEditingTarget];
    if (target && [target respondsToSelector:@selector(paste:)]) {
        [target paste:sender];
    }
}
- (void)copy:(id)sender {
    id target = [self vtr_textEditingTarget];
    if (target && [target respondsToSelector:@selector(copy:)]) {
        [target copy:sender];
    }
}
- (void)cut:(id)sender {
    id target = [self vtr_textEditingTarget];
    if (target && [target respondsToSelector:@selector(cut:)]) {
        [target cut:sender];
    }
}
- (void)selectAll:(id)sender {
    id target = [self vtr_textEditingTarget];
    if (target && [target respondsToSelector:@selector(selectAll:)]) {
        [target selectAll:sender];
    }
}

- (void)resetCursorRects {
    if (!vtr_view_reset_cursor_rects(self)) {
        [super resetCursorRects];
    }
}

- (void)cursorUpdate:(NSEvent*)event {
    if (!vtr_view_cursor_update(self)) {
        [super cursorUpdate:event];
    }
}
@end


@implementation VtrMetalView
- (BOOL)isFlipped { return YES; }
// Guest client pixels never move the window. NSView's default answers YES
// for a non-opaque view, which lets AppKit drag the window from its
// "background"; Win32 moves a window only for WM_NCHITTEST HTCAPTION
// (perform_guest_frame_mouse_down / begin_native_window_drag).
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event { (void)event; }
- (void)keyUp:(NSEvent*)event { (void)event; }
- (void)flagsChanged:(NSEvent*)event { (void)event; }

- (void)resetCursorRects {
    if (!vtr_view_reset_cursor_rects(self)) {
        [super resetCursorRects];
    }
}

- (void)cursorUpdate:(NSEvent*)event {
    if (!vtr_view_cursor_update(self)) {
        [super cursorUpdate:event];
    }
}
@end

@implementation VtrWindow
- (instancetype)initWithContentRect:(NSRect)contentRect
                          styleMask:(NSWindowStyleMask)style
                            backing:(NSBackingStoreType)backingStoreType
                              defer:(BOOL)flag {
    if ((self = [super initWithContentRect:contentRect
                                 styleMask:style
                                   backing:backingStoreType
                                     defer:flag])) {
        _allowKey = YES;
    }
    return self;
}
- (BOOL)canBecomeKeyWindow { return self.allowKey; }
- (BOOL)canBecomeMainWindow { return self.allowKey; }

- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen*)screen {
    if (self.allowOffscreenFrame) {
        return frameRect;
    }
    return [super constrainFrameRect:frameRect toScreen:screen];
}

- (void)sendEvent:(NSEvent*)event {
    const NSEventType type = [event type];
    if (type == NSEventTypeKeyDown) {
        NSEventModifierFlags modifiers = [event modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
        // Win32 dialog mnemonics are Alt-based. Treating Ctrl as a mnemonic
        // modifier swallows Ctrl-based application hotkeys (control groups,
        // for example) before the guest ever sees the key.
        if (modifiers & NSEventModifierFlagOption) {
            NSString* characters = [[event charactersIgnoringModifiers] lowercaseString];
            if ([characters length] > 0 &&
                _bridge && _bridge->activate_native_mnemonic((__bridge void*)self,
                                                              [characters characterAtIndex:0])) {
                return;
            }
        }

        // Cmd/Ctrl+C/X/V/A → native text actions. Windows users paste with
        // Ctrl+V; macOS defaults to Cmd+V. VtrContentView also forwards these
        // when it is first responder (see paste: there).
        const BOOL clip_chord =
            (modifiers & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0 &&
            (modifiers & NSEventModifierFlagOption) == 0;
        if (clip_chord) {
            NSString* characters = [[event charactersIgnoringModifiers] lowercaseString];
            const unichar ch = [characters length] > 0 ? [characters characterAtIndex:0] : 0;
            SEL action = nil;
            if (ch == 'v') action = @selector(paste:);
            else if (ch == 'c') action = @selector(copy:);
            else if (ch == 'x') action = @selector(cut:);
            else if (ch == 'a') action = @selector(selectAll:);
            if (action != nil) {
                id target = [self firstResponder];
                if ([target isKindOfClass:[VtrContentView class]]) {
                    [(VtrContentView*)target performSelector:action withObject:nil];
                    return;
                }
                if ([target respondsToSelector:action]) {
                    [NSApp sendAction:action to:target from:self];
                    return;
                }
                // Field may not be first responder yet — let content view find it.
                NSView* content = [self contentView];
                if ([content isKindOfClass:[VtrContentView class]] &&
                    [content respondsToSelector:action]) {
                    [(VtrContentView*)content performSelector:action withObject:nil];
                    return;
                }
            }
        }
    }

    if (type == NSEventTypeKeyDown ||
        type == NSEventTypeKeyUp ||
        type == NSEventTypeFlagsChanged) {
        const NSEventModifierFlags modifiers =
            [event modifierFlags] &
            NSEventModifierFlagDeviceIndependentFlagsMask;
        NSResponder* responder = [self firstResponder];
        const bool native_text_editor =
            [responder isKindOfClass:[NSTextView class]] ||
            [responder isKindOfClass:[NSTextField class]] ||
            [responder isKindOfClass:[NSText class]];

        // CocoaBridge translates these events into WM_KEYDOWN/WM_KEYUP after
        // NSApp dispatch returns. Do not also pass guest-directed keystrokes
        // through AppKit's command/responder machinery: an unhandled key
        // reaches NSResponder's default implementation and rings NSBeep.
        // Native text editors and macOS Command shortcuts retain normal host
        // behavior.
        if (!native_text_editor &&
            (modifiers & NSEventModifierFlagCommand) == 0) {
            return;
        }
    }
    [super sendEvent:event];
    if (type == NSEventTypeLeftMouseDown && _bridge)
        _bridge->finish_native_move_size_for_window((__bridge void*)self);
}
@end

@implementation VtrPanel
- (instancetype)initWithContentRect:(NSRect)contentRect
                          styleMask:(NSWindowStyleMask)style
                            backing:(NSBackingStoreType)backingStoreType
                              defer:(BOOL)flag {
    if ((self = [super initWithContentRect:contentRect
                                 styleMask:style
                                   backing:backingStoreType
                                     defer:flag])) {
        _allowKey = NO;
    }
    return self;
}
- (BOOL)canBecomeKeyWindow { return self.allowKey; }
- (BOOL)canBecomeMainWindow { return NO; }
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen*)screen {
    if (self.allowOffscreenFrame) {
        return frameRect;
    }
    return [super constrainFrameRect:frameRect toScreen:screen];
}
- (void)sendEvent:(NSEvent*)event {
    [super sendEvent:event];
    if ([event type] == NSEventTypeLeftMouseDown && _bridge)
        _bridge->finish_native_move_size_for_window((__bridge void*)self);
}
@end

// Container view used for embedded child dialogs/pages.
// It forwards clicks to its child controls, but lets clicks on empty space
// pass through to views underneath (e.g. wizard bottom button strip).
@implementation VtrContainerView
- (BOOL)isFlipped { return YES; }
// Guest client pixels never move the window. NSView's default answers YES
// for a non-opaque view, which lets AppKit drag the window from its
// "background"; Win32 moves a window only for WM_NCHITTEST HTCAPTION
// (perform_guest_frame_mouse_down / begin_native_window_drag).
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (NSView*)hitTest:(NSPoint)point {
    NSView* hit = [super hitTest:point];
    return (hit == self) ? nil : hit;
}
- (void)resetCursorRects {
    if (!vtr_view_reset_cursor_rects(self)) {
        [super resetCursorRects];
    }
}
- (void)cursorUpdate:(NSEvent*)event {
    if (!vtr_view_cursor_update(self)) {
        [super cursorUpdate:event];
    }
}
@end
