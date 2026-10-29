// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_controls.mm
/// @brief Win32 common controls backed by AppKit views.
///
/// ComboBox, tab control, list box, list view, and tree view. Each maps a
/// Win32 control's message-and-index model onto an AppKit view that does not
/// share it: CB_ADDSTRING onto NSPopUpButton, LVM_INSERTITEM onto an
/// NSTableView, TVM_INSERTITEM onto an NSOutlineView.
///
/// The translation is the substance here. Win32 addresses items by integer
/// index and reports events by posting WM_COMMAND/WM_NOTIFY to a parent HWND;
/// AppKit addresses them by object and reports by delegate callback. Every
/// method below sits on that seam, which is why they belong together rather
/// than with window management.
///
/// Split out of cocoa_bridge.mm. These are CocoaBridge methods defined in a
/// separate translation unit; all are already declared in cocoa_bridge.hpp, so
/// this file adds no interface of its own.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
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

#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_bridge.hpp"

@interface VtrWindowScroller : NSScroller
@property(nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@property(nonatomic, assign) vitreo::windowing::HWND hwnd;
@property(nonatomic, assign) unsigned axis;
@property(nonatomic, assign) int minimum;
@property(nonatomic, assign) int maximumPosition;
@property(nonatomic, assign) int trackPosition;
@property(nonatomic, assign) BOOL trackingThumb;
- (void)scrollAction:(id)sender;
@end
@implementation VtrWindowScroller
- (void)postCode:(unsigned)code {
    if (_bridge) _bridge->post_window_message(_hwnd, _axis ? 0x115 : 0x114,
        code | (static_cast<unsigned long long>(static_cast<unsigned short>(_trackPosition)) << 16), 0);
}
- (void)scrollAction:(id)sender {
    (void)sender;
    unsigned code;
    switch (self.hitPart) {
        case NSScrollerDecrementLine: code = 0; break;
        case NSScrollerIncrementLine: code = 1; break;
        case NSScrollerDecrementPage: code = 2; break;
        case NSScrollerIncrementPage: code = 3; break;
        case NSScrollerKnob: case NSScrollerKnobSlot:
            _trackingThumb = YES;
            _trackPosition = static_cast<int>(std::llround(_minimum + self.doubleValue * (static_cast<double>(_maximumPosition) - _minimum)));
            code = 5; break;
        default: return;
    }
    [self postCode:code];
}
- (void)mouseDown:(NSEvent*)event {
    [super mouseDown:event];
    if (_trackingThumb) [self postCode:4];
    [self postCode:8];
    // Keep the final tracking position available while queued messages drain.
}
@end

@interface VtrToolbarAction : NSObject
@property(nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@property(nonatomic, assign) vitreo::windowing::HWND parent;
@property(nonatomic, assign) vitreo::windowing::HWND toolbar;
- (void)clicked:(NSButton*)sender;
@end
@implementation VtrToolbarAction
- (void)clicked:(NSButton*)sender {
    if (_bridge) _bridge->post_window_message(_parent, vitreo::windowing::WM::COMMAND,
                                              sender.tag, _toolbar);
}
@end

namespace vitreo::windowing {

bool CocoaBridge::update_window_scrollbar(HWND hwnd, u32 bar, const ScrollBarState& state, bool redraw) {
    if (bar > 1) return false;
    __block bool ok = false;
    vtr_run_on_main_sync(^{
        NSView* view = nil;
        auto control = m_controls.find(hwnd);
        if (control != m_controls.end()) view = (__bridge NSView*)control->second;
        if (!view) {
            auto window = m_windows.find(hwnd);
            if (window != m_windows.end()) view = [(__bridge NSWindow*)window->second.ns_window contentView];
        }
        if (!view) return;
        const u64 key = (static_cast<u64>(hwnd) << 1) | bar;
        auto found = m_window_scrollers.find(key);
        VtrWindowScroller* scroller = found == m_window_scrollers.end() ? nil : (__bridge VtrWindowScroller*)found->second;
        if (!scroller && state.visible) {
            const CGFloat thickness = 20; // USER32 SM_CXVSCROLL / SM_CYHSCROLL
            const NSRect bounds = view.bounds;
            const CGFloat y = view.isFlipped ? bounds.size.height - thickness : 0;
            scroller = [[VtrWindowScroller alloc] initWithFrame:bar
                ? NSMakeRect(bounds.size.width - thickness, 0, thickness, bounds.size.height)
                : NSMakeRect(0, y, bounds.size.width, thickness)];
            scroller.scrollerStyle = NSScrollerStyleLegacy;
            scroller.autoresizingMask = bar ? NSViewMinXMargin | NSViewHeightSizable
                : NSViewWidthSizable | (view.isFlipped ? NSViewMinYMargin : NSViewMaxYMargin);
            scroller.bridge = this; scroller.hwnd = hwnd; scroller.axis = bar;
            scroller.target = scroller; scroller.action = @selector(scrollAction:);
            [view addSubview:scroller positioned:NSWindowAbove relativeTo:nil];
            m_window_scrollers[key] = (__bridge void*)scroller;
            [scroller release];
        }
        if (scroller) {
            scroller.hidden = !state.visible;
            scroller.enabled = state.enabled;
            scroller.minimum = state.minimum;
            scroller.maximumPosition = static_cast<i32>(static_cast<i64>(state.maximum) - (state.page ? state.page - 1 : 0));
            const double count = static_cast<double>(state.maximum) - state.minimum + 1;
            scroller.knobProportion = count > 0 ? std::min(1.0, state.page / count) : 1;
            const double range = static_cast<double>(scroller.maximumPosition) - state.minimum;
            scroller.doubleValue = range > 0 ? (state.position - static_cast<double>(state.minimum)) / range : 0;
            if (redraw) scroller.needsDisplay = YES;
        }
        ok = true;
    });
    return ok;
}

bool CocoaBridge::get_scrollbar_tracking(HWND hwnd, u32 bar, i32& position) const {
    __block bool ok = false; __block i32 value = 0;
    vtr_run_on_main_sync(^{
        auto it = m_window_scrollers.find((static_cast<u64>(hwnd) << 1) | bar);
        if (it == m_window_scrollers.end()) return;
        VtrWindowScroller* scroller = (__bridge VtrWindowScroller*)it->second;
        if (!scroller.trackingThumb) return;
        value = scroller.trackPosition; ok = true;
    });
    if (ok) position = value;
    return ok;
}

void CocoaBridge::forget_common_control(HWND hwnd) {
    for (u32 bar = 0; bar < 2; ++bar) {
        auto it = m_window_scrollers.find((static_cast<u64>(hwnd) << 1) | bar);
        if (it != m_window_scrollers.end()) {
            [(__bridge NSView*)it->second removeFromSuperview];
            m_window_scrollers.erase(it);
        }
    }
    if (auto it = m_toolbars.find(hwnd); it != m_toolbars.end()) {
        for (void* image : it->second.images) if (image) CFRelease((CFTypeRef)image);
        m_toolbars.erase(it);
    }
    m_status_parts.erase(hwnd);
    m_combo_item_data.erase(hwnd);
}

bool CocoaBridge::rebar_attach_child(HWND rebar, HWND child) {
    __block bool result = false;
    vtr_run_on_main_sync(^{
        auto parent = m_controls.find(rebar);
        auto item = m_controls.find(child);
        if (parent == m_controls.end() || item == m_controls.end()) return;
        NSView* container = (__bridge NSView*)parent->second;
        NSView* view = (__bridge NSView*)item->second;
        [view removeFromSuperview];
        [container addSubview:view];
        view.frame = container.bounds;
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        result = true;
    });
    return result;
}

i32 CocoaBridge::toolbar_add_bitmap(HWND hwnd, u32 bitmap, u32 count) {
    __block i32 result = -1;
    vtr_run_on_main_sync(^{
        auto bit = m_bitmaps.find(bitmap);
        auto it = m_toolbars.find(hwnd);
        if (bit == m_bitmaps.end() || it == m_toolbars.end() || !count || count > 4096) return;
        NSImage* strip = (__bridge NSImage*)bit->second;
        CGImageRef source = [strip CGImageForProposedRect:nullptr context:nil hints:nil];
        if (!source || CGImageGetWidth(source) < count) return;
        const usize width = CGImageGetWidth(source) / count;
        const usize height = CGImageGetHeight(source);
        if (!width || !height) return;
        result = static_cast<i32>(it->second.images.size());
        for (u32 i = 0; i < count; ++i) {
            NSBitmapImageRep* pixels = [[NSBitmapImageRep alloc]
                initWithBitmapDataPlanes:nil pixelsWide:width pixelsHigh:height bitsPerSample:8
                samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                bitmapFormat:0 bytesPerRow:width * 4 bitsPerPixel:32];
            CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
            CGContextRef context = CGBitmapContextCreate(pixels.bitmapData, width, height, 8,
                pixels.bytesPerRow, colors, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(colors);
            if (!context) { [pixels release]; result = -1; return; }
            CGContextSetBlendMode(context, kCGBlendModeCopy);
            CGContextDrawImage(context, CGRectMake(-static_cast<double>(i * width), 0,
                CGImageGetWidth(source), height), source);
            CGContextRelease(context);
            for (usize y = 0; y < height; ++y) for (usize x = 0; x < width; ++x) {
                unsigned char* rgba = pixels.bitmapData + y * pixels.bytesPerRow + x * 4;
                // Mapped COLOR_BTNFACE is transparent. Preserve genuine alpha
                // and every other image color rather than guessing from a corner.
                if (rgba[3] == 255 && rgba[0] == 240 && rgba[1] == 240 && rgba[2] == 240)
                    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
            }
            NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
            [image addRepresentation:pixels]; [pixels release];
            // This translation unit uses manual reference counting. Transfer
            // the alloc reference to the toolbar; forget_common_control releases it.
            it->second.images.push_back((__bridge void*)image);
        }
    });
    return result;
}

void CocoaBridge::render_toolbar(HWND hwnd) {
    vtr_run_on_main_sync(^{
        auto view_it = m_controls.find(hwnd);
        auto it = m_toolbars.find(hwnd);
        auto control = m_control_state.find(hwnd);
        if (view_it == m_controls.end() || it == m_toolbars.end() || control == m_control_state.end()) return;
        NSView* view = (__bridge NSView*)view_it->second;
        for (NSView* child in [view.subviews copy]) [child removeFromSuperview];
        CGFloat x = 0;
        for (const auto& item : it->second.buttons) {
            if (item.state & 8) continue; // TBSTATE_HIDDEN
            if (item.style & 1) {
                const CGFloat width = item.image > 0 ? item.image : 8;
                NSBox* divider = [[NSBox alloc] initWithFrame:NSMakeRect(x + width / 2, 3, 1, std::max(1, it->second.height - 2))];
                divider.boxType = NSBoxSeparator;
                [view addSubview:divider];
                x += width;
                continue;
            }
            NSButton* button = [[NSButton alloc] initWithFrame:NSMakeRect(x, 2, it->second.width, it->second.height)];
            button.bordered = NO;
            button.title = to_nsstring(item.text);
            button.enabled = (item.state & 4) != 0;
            button.state = (item.state & 1) ? NSControlStateValueOn : NSControlStateValueOff;
            button.tag = item.command;
            if (item.image >= 0 && static_cast<usize>(item.image) < it->second.images.size()) {
                button.image = (__bridge NSImage*)it->second.images[item.image];
                button.imagePosition = NSImageOnly;
            }
            VtrToolbarAction* action = [[VtrToolbarAction alloc] init];
            action.bridge = this;
            action.parent = control->second.parent;
            action.toolbar = hwnd;
            button.target = action;
            button.action = @selector(clicked:);
            static char actionKey;
            objc_setAssociatedObject(button, &actionKey, action, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [view addSubview:button];
            x += it->second.width;
        }
    });
}

bool CocoaBridge::toolbar_insert_buttons(HWND hwnd, i32 index, const std::vector<ToolbarButton>& buttons) {
    auto it = m_toolbars.find(hwnd);
    if (it == m_toolbars.end()) return false;
    if (index < 0 || static_cast<usize>(index) > it->second.buttons.size()) index = it->second.buttons.size();
    it->second.buttons.insert(it->second.buttons.begin() + index, buttons.begin(), buttons.end());
    render_toolbar(hwnd);
    return true;
}

bool CocoaBridge::toolbar_set_state(HWND hwnd, u32 command, u8 mask, bool on) {
    auto it = m_toolbars.find(hwnd);
    if (it == m_toolbars.end()) return false;
    for (auto& button : it->second.buttons) if (button.command == command) {
        const u8 next = on ? button.state | mask : button.state & ~mask;
        if (next == button.state) return true;
        button.state = next;
        render_toolbar(hwnd);
        return true;
    }
    return false;
}

void CocoaBridge::toolbar_set_size(HWND hwnd, i32 width, i32 height) {
    auto it = m_toolbars.find(hwnd);
    if (it == m_toolbars.end()) return;
    it->second.width = width; it->second.height = height;
    render_toolbar(hwnd);
}

i32 CocoaBridge::toolbar_width(HWND hwnd) const {
    auto it = m_toolbars.find(hwnd);
    if (it == m_toolbars.end()) return 0;
    i32 width = 0;
    for (const auto& button : it->second.buttons) if (!(button.state & 8))
        width += (button.style & 1) ? (button.image > 0 ? button.image : 8) : it->second.width;
    return width;
}

std::vector<CocoaBridge::ToolbarButton> CocoaBridge::toolbar_buttons(HWND hwnd) const {
    auto it = m_toolbars.find(hwnd);
    return it == m_toolbars.end() ? std::vector<ToolbarButton>{} : it->second.buttons;
}

void CocoaBridge::render_status_parts(HWND hwnd) {
    vtr_run_on_main_sync(^{
        auto it = m_controls.find(hwnd);
        auto state = m_status_parts.find(hwnd);
        if (it == m_controls.end() || state == m_status_parts.end()) return;
        NSView* view = (__bridge NSView*)it->second;
        for (NSView* child in [view.subviews copy]) [child removeFromSuperview];
        CGFloat left = 0;
        for (usize i = 0; i < state->second.edges.size(); ++i) {
            CGFloat right = state->second.edges[i] < 0 ? view.bounds.size.width : state->second.edges[i];
            NSTextField* label = [NSTextField labelWithString:to_nsstring(state->second.text[i])];
            label.font = [NSFont systemFontOfSize:11];
            label.frame = NSMakeRect(left + 4, 2, std::max<CGFloat>(0, right - left - 8), 18);
            label.lineBreakMode = NSLineBreakByTruncatingTail;
            [view addSubview:label];
            left = right;
        }
    });
}

bool CocoaBridge::status_set_parts(HWND hwnd, const std::vector<i32>& edges) {
    if (edges.empty() || edges.size() > 256 || !m_controls.contains(hwnd)) return false;
    auto& state = m_status_parts[hwnd];
    state.edges = edges;
    state.text.resize(edges.size());
    render_status_parts(hwnd);
    return true;
}

bool CocoaBridge::status_set_text(HWND hwnd, u32 part, const std::wstring& text) {
    auto it = m_status_parts.find(hwnd);
    if (it == m_status_parts.end() || part >= it->second.text.size()) return false;
    if (it->second.text[part] == text) return true;
    it->second.text[part] = text;
    render_status_parts(hwnd);
    return true;
}

std::wstring CocoaBridge::status_get_text(HWND hwnd, u32 part) const {
    auto it = m_status_parts.find(hwnd);
    return it != m_status_parts.end() && part < it->second.text.size() ? it->second.text[part] : L"";
}

std::vector<i32> CocoaBridge::status_get_parts(HWND hwnd) const {
    auto it = m_status_parts.find(hwnd);
    return it != m_status_parts.end() ? it->second.edges : std::vector<i32>{};
}

// =============================================================================
// ComboBox (NSPopUpButton) support
// =============================================================================

i32 CocoaBridge::combo_add_item(HWND ctrl_hwnd, const std::wstring& text, i32 index) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return -1;
    }

    NSView* view = (__bridge NSView*)it->second;
    if ([view isKindOfClass:[NSComboBox class]]) {
        NSComboBox* combo = (NSComboBox*)view;
        __block i32 result = -1;
        vtr_run_on_main_sync(^{
            i32 count = static_cast<i32>(combo.numberOfItems);
            result = index >= 0 && index <= count ? index : count;
            [combo insertItemWithObjectValue:to_nsstring(text) atIndex:result];
            auto& data = m_combo_item_data[ctrl_hwnd];
            data.insert(data.begin() + result, 0);
        });
        return result;
    }
    if (![view isKindOfClass:[NSPopUpButton class]]) {
        return -1;
    }

    NSPopUpButton* popup = (NSPopUpButton*)view;
    NSString* ns_text = to_nsstring(text);
    __block i32 out_index = -1;

    void (^updateBlock)(void) = ^{
        NSInteger count = [popup numberOfItems];
        if (index >= 0 && index <= static_cast<i32>(count)) {
            [popup insertItemWithTitle:ns_text atIndex:index];
            out_index = index;
        } else {
            [popup addItemWithTitle:ns_text];
            out_index = static_cast<i32>([popup numberOfItems]) - 1;
        }
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_sync(dispatch_get_main_queue(), updateBlock);
    }

    return out_index;
}

bool CocoaBridge::combo_reset_content(HWND ctrl_hwnd) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return false;
    }

    NSView* view = (__bridge NSView*)it->second;
    if ([view isKindOfClass:[NSComboBox class]]) {
        NSComboBox* combo = (NSComboBox*)view;
        vtr_run_on_main_sync(^{ [combo removeAllItems]; [combo setStringValue:@""]; m_combo_item_data[ctrl_hwnd].clear(); });
        return true;
    }
    if (![view isKindOfClass:[NSPopUpButton class]]) {
        return false;
    }

    NSPopUpButton* popup = (NSPopUpButton*)view;
    void (^updateBlock)(void) = ^{
        [popup removeAllItems];
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_sync(dispatch_get_main_queue(), updateBlock);
    }

    return true;
}

i32 CocoaBridge::combo_get_count(HWND ctrl_hwnd) const {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return -1;
    }

    NSView* view = (__bridge NSView*)it->second;
    if ([view isKindOfClass:[NSComboBox class]]) {
        NSComboBox* combo = (NSComboBox*)view;
        __block i32 result;
        vtr_run_on_main_sync(^{ result = static_cast<i32>(combo.numberOfItems); });
        return result;
    }
    if (![view isKindOfClass:[NSPopUpButton class]]) {
        return -1;
    }

    NSPopUpButton* popup = (NSPopUpButton*)view;
    __block i32 out_count = -1;

    void (^readBlock)(void) = ^{
        out_count = static_cast<i32>([popup numberOfItems]);
    };

    if ([NSThread isMainThread]) {
        readBlock();
    } else {
        dispatch_sync(dispatch_get_main_queue(), readBlock);
    }

    return out_count;
}

i32 CocoaBridge::combo_set_cur_sel(HWND ctrl_hwnd, i32 index) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return -1;
    }

    NSView* view = (__bridge NSView*)it->second;
    if ([view isKindOfClass:[NSComboBox class]]) {
        NSComboBox* combo = (NSComboBox*)view;
        __block i32 result = -1;
        vtr_run_on_main_sync(^{
            if (index >= 0 && index < combo.numberOfItems) {
                [combo selectItemAtIndex:index];
                [combo setStringValue:[combo itemObjectValueAtIndex:index]];
                result = index;
            } else {
                if (combo.indexOfSelectedItem >= 0) [combo deselectItemAtIndex:combo.indexOfSelectedItem];
                [combo setStringValue:@""];
            }
        });
        return result;
    }
    if (![view isKindOfClass:[NSPopUpButton class]]) {
        return -1;
    }

    NSPopUpButton* popup = (NSPopUpButton*)view;
    __block i32 out_sel = -1;

    void (^updateBlock)(void) = ^{
        NSInteger count = [popup numberOfItems];
        if (index < 0 || index >= static_cast<i32>(count)) {
            [popup selectItem:nil];
            out_sel = -1;
            return;
        }
        [popup selectItemAtIndex:index];
        out_sel = static_cast<i32>([popup indexOfSelectedItem]);
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_sync(dispatch_get_main_queue(), updateBlock);
    }

    return out_sel;
}

i32 CocoaBridge::combo_get_cur_sel(HWND ctrl_hwnd) const {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return -1;
    }

    NSView* view = (__bridge NSView*)it->second;
    if ([view isKindOfClass:[NSComboBox class]]) {
        NSComboBox* combo = (NSComboBox*)view;
        __block i32 result;
        vtr_run_on_main_sync(^{ result = static_cast<i32>(combo.indexOfSelectedItem); });
        return result;
    }
    if (![view isKindOfClass:[NSPopUpButton class]]) {
        return -1;
    }

    NSPopUpButton* popup = (NSPopUpButton*)view;
    __block i32 out_sel = -1;

    void (^readBlock)(void) = ^{
        out_sel = static_cast<i32>([popup indexOfSelectedItem]);
    };

    if ([NSThread isMainThread]) {
        readBlock();
    } else {
        dispatch_sync(dispatch_get_main_queue(), readBlock);
    }

    return out_sel;
}

std::wstring CocoaBridge::combo_get_item_text(HWND ctrl_hwnd, i32 index) const {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return {};
    }

    NSView* view = (__bridge NSView*)it->second;
    if ([view isKindOfClass:[NSComboBox class]]) {
        NSComboBox* combo = (NSComboBox*)view;
        __block NSString* result = nil;
        // An owned copy: the main thread's pool may drain the item first.
        vtr_run_on_main_sync(^{ if (index >= 0 && index < combo.numberOfItems) result = [[combo itemObjectValueAtIndex:index] copy]; });
        std::wstring text = result ? from_nsstring(result) : std::wstring{};
        [result release];
        return text;
    }
    if (![view isKindOfClass:[NSPopUpButton class]]) {
        return {};
    }

    NSPopUpButton* popup = (NSPopUpButton*)view;
    __block NSString* ns_text = nil;

    void (^readBlock)(void) = ^{
        NSInteger count = [popup numberOfItems];
        if (index < 0 || index >= static_cast<i32>(count)) {
            ns_text = nil;
            return;
        }
        ns_text = [[[popup itemAtIndex:index] title] copy];
    };

    if ([NSThread isMainThread]) {
        readBlock();
    } else {
        dispatch_sync(dispatch_get_main_queue(), readBlock);
    }

    std::wstring text = (ns_text && [ns_text length] != 0)
        ? from_nsstring(ns_text) : std::wstring{};
    [ns_text release];
    return text;
}

bool CocoaBridge::combo_set_item_data(HWND ctrl_hwnd, i32 index, u64 data) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return false;
    }

    NSView* view = (__bridge NSView*)it->second;
    if ([view isKindOfClass:[NSComboBox class]]) {
        NSComboBox* combo = (NSComboBox*)view;
        auto& data_items = m_combo_item_data[ctrl_hwnd];
        if (index < 0 || static_cast<usize>(index) >= data_items.size()) return false;
        data_items[index] = data;
        return true;
    }
    if (![view isKindOfClass:[NSPopUpButton class]]) {
        return false;
    }

    NSPopUpButton* popup = (NSPopUpButton*)view;
    __block bool ok = false;

    void (^updateBlock)(void) = ^{
        NSInteger count = [popup numberOfItems];
        if (index < 0 || index >= static_cast<i32>(count)) {
            ok = false;
            return;
        }
        // NSMenuItem.tag is NSInteger — wide enough for 32-bit guest LPARAMs.
        [[popup itemAtIndex:index] setTag:static_cast<NSInteger>(data)];
        ok = true;
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_sync(dispatch_get_main_queue(), updateBlock);
    }

    return ok;
}

u64 CocoaBridge::combo_get_item_data(HWND ctrl_hwnd, i32 index, bool* ok) const {
    if (ok) *ok = false;
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) {
        return 0;
    }

    NSView* view = (__bridge NSView*)it->second;
    if ([view isKindOfClass:[NSComboBox class]]) {
        NSComboBox* combo = (NSComboBox*)view;
        auto data_it = m_combo_item_data.find(ctrl_hwnd);
        if (data_it == m_combo_item_data.end() || index < 0 || static_cast<usize>(index) >= data_it->second.size()) return 0;
        if (ok) *ok = true;
        return data_it->second[index];
    }
    if (![view isKindOfClass:[NSPopUpButton class]]) {
        return 0;
    }

    NSPopUpButton* popup = (NSPopUpButton*)view;
    __block u64 out = 0;
    __block bool found = false;

    void (^readBlock)(void) = ^{
        NSInteger count = [popup numberOfItems];
        if (index < 0 || index >= static_cast<i32>(count)) {
            found = false;
            return;
        }
        out = static_cast<u64>([[popup itemAtIndex:index] tag]);
        found = true;
    };

    if ([NSThread isMainThread]) {
        readBlock();
    } else {
        dispatch_sync(dispatch_get_main_queue(), readBlock);
    }

    if (ok) *ok = found;
    return out;
}

// =============================================================================
// Tab control (VtrTabStripView) support
// =============================================================================

i32 CocoaBridge::tab_insert_item(
    HWND ctrl_hwnd, i32 index, const std::wstring& text, u64 lparam) {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || it->second.tab_view == nullptr) {
        return -1;
    }

    auto& labels = it->second.labels;
    auto& lparams = it->second.lparams;
    i32 target = index;
    if (target < 0 || target > static_cast<i32>(labels.size())) {
        target = static_cast<i32>(labels.size());
    }
    labels.insert(labels.begin() + static_cast<std::ptrdiff_t>(target), text);
    lparams.insert(lparams.begin() + static_cast<std::ptrdiff_t>(target),
                   lparam);
    if (it->second.selected < 0 && !labels.empty()) {
        it->second.selected = 0;
    }

    VtrTabStripView* strip = (__bridge VtrTabStripView*)it->second.tab_view;
    void (^reload)(void) = ^{ [strip reloadTabs]; };
    if ([NSThread isMainThread]) reload();
    else dispatch_sync(dispatch_get_main_queue(), reload);
    return target;
}

bool CocoaBridge::tab_delete_item(HWND ctrl_hwnd, i32 index) {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || !it->second.tab_view || index < 0 ||
        index >= static_cast<i32>(it->second.labels.size())) return false;
    auto& tab = it->second;
    tab.labels.erase(tab.labels.begin() + index);
    tab.lparams.erase(tab.lparams.begin() + index);
    if (tab.selected == index) tab.selected = -1;
    else if (tab.selected > index) --tab.selected;
    VtrTabStripView* strip = (__bridge VtrTabStripView*)tab.tab_view;
    vtr_run_on_main_sync(^{ [strip reloadTabs]; });
    return true;
}

i32 CocoaBridge::tab_get_item_count(HWND ctrl_hwnd) const {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || it->second.tab_view == nullptr) return -1;
    return static_cast<i32>(it->second.labels.size());
}

i32 CocoaBridge::tab_get_cur_sel(HWND ctrl_hwnd) const {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || it->second.tab_view == nullptr) return -1;
    if (it->second.labels.empty()) return -1;
    return it->second.selected;
}

i32 CocoaBridge::tab_set_cur_sel(HWND ctrl_hwnd, i32 index) {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || it->second.tab_view == nullptr) return -1;
    const i32 previous = it->second.selected;
    if (index >= 0 && index < static_cast<i32>(it->second.labels.size())) {
        it->second.selected = index;
        for (usize p = 0; p < it->second.page_views.size(); ++p) {
            NSView* page = (__bridge NSView*)it->second.page_views[p];
            if (page) {
                [page setHidden:(static_cast<i32>(p) != index)];
            }
        }
        VtrTabStripView* strip = (__bridge VtrTabStripView*)it->second.tab_view;
        void (^reload)(void) = ^{ [strip reloadTabs]; };
        if ([NSThread isMainThread]) reload();
        else dispatch_sync(dispatch_get_main_queue(), reload);
    }
    return previous;
}

void CocoaBridge::draw_tab_strip(HWND ctrl_hwnd) {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || it->second.tab_view == nullptr) return;
    VtrTabStripView* strip = (__bridge VtrTabStripView*)it->second.tab_view;
    const CGFloat strip_h = 26.0;
    [[NSColor windowBackgroundColor] setFill];
    NSRectFill(NSMakeRect(0, 0, strip.bounds.size.width, strip_h));
    [[NSColor separatorColor] setStroke];
    NSBezierPath* baseline = [NSBezierPath bezierPath];
    [baseline moveToPoint:NSMakePoint(0, strip_h - 0.5)];
    [baseline lineToPoint:NSMakePoint(strip.bounds.size.width, strip_h - 0.5)];
    [baseline stroke];

    NSFont* font = [NSFont systemFontOfSize:11.0];
    for (usize i = 0; i < it->second.labels.size(); ++i) {
        Rect item{};
        if (!tab_get_item_rect(ctrl_hwnd, static_cast<i32>(i), item)) continue;
        const NSRect r = NSMakeRect(item.left, item.top,
                                    item.right - item.left,
                                    item.bottom - item.top);
        const bool on = static_cast<i32>(i) == it->second.selected;
        [(on ? [NSColor controlBackgroundColor]
             : [NSColor windowBackgroundColor]) setFill];
        NSBezierPath* path =
            [NSBezierPath bezierPathWithRoundedRect:r xRadius:3 yRadius:3];
        [path fill];
        [[NSColor separatorColor] setStroke];
        [path stroke];
        NSString* title = to_nsstring(it->second.labels[i]);
        NSDictionary* attrs = @{
            NSFontAttributeName : font,
            NSForegroundColorAttributeName : [NSColor labelColor],
        };
        const NSSize textSize = [title sizeWithAttributes:attrs];
        const NSPoint origin = NSMakePoint(
            r.origin.x + (r.size.width - textSize.width) * 0.5,
            r.origin.y + (r.size.height - textSize.height) * 0.5);
        [title drawAtPoint:origin withAttributes:attrs];
    }
}

void CocoaBridge::tab_strip_clicked(HWND ctrl_hwnd, i32 index, bool notify) {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end()) return;
    if (index < 0 || index >= static_cast<i32>(it->second.labels.size())) {
        return;
    }
    if (it->second.selected == index) return;
    if (notify) {
        enqueue_tab_notify(ctrl_hwnd, true);
    }
    (void)tab_set_cur_sel(ctrl_hwnd, index);
    if (notify) {
        enqueue_tab_notify(ctrl_hwnd, false);
    }
}

namespace {
constexpr u64 kTabNotifyMarkerBase = 0x5654525400000000ULL;
constexpr u64 kTabNotifyMarkerMask = 0xFFFFFFFE00000000ULL;
constexpr u64 kTabNotifyChangingBit = 0x0000000100000000ULL;
}

void CocoaBridge::enqueue_tab_notify(
    HWND ctrl_hwnd, bool selection_changing) {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || !m_queue ||
        it->second.parent == NULL_HWND ||
        it->second.parent == HWND_DESKTOP) {
        return;
    }

    MSG msg{};
    msg.hwnd = it->second.parent;
    msg.message = WM::NOTIFY;
    msg.wParam = it->second.ctrl_id;
    msg.lParam = kTabNotifyMarkerBase |
        (selection_changing ? kTabNotifyChangingBit : 0) |
        static_cast<u64>(ctrl_hwnd);
    msg.time = static_cast<u32>(continuous_tick_count_ms());
    m_queue->post(msg);

    VITREO_TRACE(
        "[cocoa] tab notify parent=0x%x ctrl=0x%x id=%u code=%s\n",
        msg.hwnd, ctrl_hwnd, it->second.ctrl_id,
        selection_changing ? "TCN_SELCHANGING" : "TCN_SELCHANGE");
}

bool CocoaBridge::decode_tab_notify_lparam(
    u64 lparam, HWND* ctrl_out, bool* selection_changing_out) noexcept {
    if ((lparam & kTabNotifyMarkerMask) != kTabNotifyMarkerBase) {
        return false;
    }
    if (ctrl_out) {
        *ctrl_out = static_cast<HWND>(lparam & 0xFFFFFFFFULL);
    }
    if (selection_changing_out) {
        *selection_changing_out =
            (lparam & kTabNotifyChangingBit) != 0;
    }
    return true;
}

bool CocoaBridge::tab_get_item_rect(
    HWND ctrl_hwnd, i32 index, Rect& rect) const {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || index < 0 ||
        static_cast<usize>(index) >= it->second.labels.size()) {
        return false;
    }

    i32 left = 4;
    for (i32 current = 0; current < index; ++current) {
        const auto length =
            it->second.labels[static_cast<usize>(current)].size();
        left += std::max<i32>(48, 18 + static_cast<i32>(length) * 7);
    }
    const auto length = it->second.labels[static_cast<usize>(index)].size();
    const i32 width = std::max<i32>(48, 18 + static_cast<i32>(length) * 7);
    rect = Rect{left, 2, left + width, 24};
    return true;
}

bool CocoaBridge::tab_get_item_text(
    HWND ctrl_hwnd, i32 index, std::wstring& text_out) const {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || index < 0 ||
        static_cast<usize>(index) >= it->second.labels.size()) {
        return false;
    }
    text_out = it->second.labels[static_cast<usize>(index)];
    return true;
}

bool CocoaBridge::tab_get_item_lparam(
    HWND ctrl_hwnd, i32 index, u64& lparam_out) const {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || index < 0 ||
        static_cast<usize>(index) >= it->second.lparams.size()) {
        return false;
    }
    lparam_out = it->second.lparams[static_cast<usize>(index)];
    return true;
}

bool CocoaBridge::tab_set_item(
    HWND ctrl_hwnd, i32 index,
    const std::optional<std::wstring>& text,
    const std::optional<u64>& lparam) {
    auto it = m_tab_controls.find(ctrl_hwnd);
    if (it == m_tab_controls.end() || it->second.tab_view == nullptr ||
        index < 0) {
        return false;
    }
    const usize idx = static_cast<usize>(index);
    if (text.has_value() && idx >= it->second.labels.size()) {
        return false;
    }
    if (lparam.has_value() && idx >= it->second.lparams.size()) {
        return false;
    }
    if (text.has_value()) {
        it->second.labels[idx] = *text;
        VtrTabStripView* strip = (__bridge VtrTabStripView*)it->second.tab_view;
        void (^reload)(void) = ^{ [strip reloadTabs]; };
        if ([NSThread isMainThread]) reload();
        else dispatch_sync(dispatch_get_main_queue(), reload);
    }
    if (lparam.has_value()) {
        it->second.lparams[idx] = *lparam;
    }
    return true;
}

// =============================================================================
// ListBox (NSScrollView + NSTableView) support
// =============================================================================

// Reload the NSTableView that backs a Win32 LISTBOX (the m_controls entry is the
// NSScrollView; its documentView is the table). Safe if the view isn't a table.
static void vtr_reload_listbox(void* scroll_view_ptr) {
    if (!scroll_view_ptr) return;
    NSView* v = (__bridge NSView*)scroll_view_ptr;
    if (![v isKindOfClass:[NSScrollView class]]) return;
    NSTableView* table = (NSTableView*)[(NSScrollView*)v documentView];
    if (![table isKindOfClass:[NSTableView class]]) return;
    void (^b)(void) = ^{ [table reloadData]; };
    if ([NSThread isMainThread]) b(); else dispatch_async(dispatch_get_main_queue(), b);
}

i32 CocoaBridge::listbox_add_item(HWND ctrl_hwnd, const std::wstring& text, i32 index) {
    auto ctrl_it = m_controls.find(ctrl_hwnd);
    if (ctrl_it == m_controls.end()) return -1;

    auto& items = m_listbox_items[ctrl_hwnd];
    i32 insert_index = index;
    if (insert_index < 0 || insert_index > static_cast<i32>(items.size())) {
        insert_index = static_cast<i32>(items.size());
    }
    items.insert(items.begin() + insert_index, text);

    auto sel_it = m_listbox_selection.find(ctrl_hwnd);
    if (sel_it == m_listbox_selection.end()) {
        m_listbox_selection[ctrl_hwnd] = -1;
    } else if (sel_it->second >= insert_index) {
        sel_it->second += 1;
    }

    vtr_reload_listbox(ctrl_it->second);
    return insert_index;
}

i32 CocoaBridge::listbox_delete_item(HWND ctrl_hwnd, i32 index) {
    auto items_it = m_listbox_items.find(ctrl_hwnd);
    auto ctrl_it = m_controls.find(ctrl_hwnd);
    if (items_it == m_listbox_items.end() || ctrl_it == m_controls.end()) return -1;

    auto& items = items_it->second;
    if (index < 0 || index >= static_cast<i32>(items.size())) return -1;

    items.erase(items.begin() + index);

    i32& sel = m_listbox_selection[ctrl_hwnd];
    if (sel == index) {
        sel = -1;
    } else if (sel > index) {
        sel -= 1;
    }

    vtr_reload_listbox(ctrl_it->second);
    return static_cast<i32>(items.size());
}

bool CocoaBridge::listbox_reset_content(HWND ctrl_hwnd) {
    auto ctrl_it = m_controls.find(ctrl_hwnd);
    if (ctrl_it == m_controls.end()) return false;

    auto& items = m_listbox_items[ctrl_hwnd];
    items.clear();
    m_listbox_selection[ctrl_hwnd] = -1;

    vtr_reload_listbox(ctrl_it->second);
    return true;
}

i32 CocoaBridge::listbox_get_count(HWND ctrl_hwnd) const {
    auto it = m_listbox_items.find(ctrl_hwnd);
    if (it == m_listbox_items.end()) return 0;
    return static_cast<i32>(it->second.size());
}

i32 CocoaBridge::listbox_set_cur_sel(HWND ctrl_hwnd, i32 index) {
    auto it = m_listbox_items.find(ctrl_hwnd);
    if (it == m_listbox_items.end()) return -1;

    i32 count = static_cast<i32>(it->second.size());
    if (index < 0 || index >= count) {
        m_listbox_selection[ctrl_hwnd] = -1;
        return -1;
    }

    m_listbox_selection[ctrl_hwnd] = index;

    // Reflect the programmatic selection in the native table (suppress the
    // delegate's notification so we don't echo a spurious LBN_SELCHANGE back).
    auto ctrl_it = m_controls.find(ctrl_hwnd);
    if (ctrl_it != m_controls.end()) {
        NSView* v = (__bridge NSView*)ctrl_it->second;
        if ([v isKindOfClass:[NSScrollView class]]) {
            NSTableView* table = (NSTableView*)[(NSScrollView*)v documentView];
            if ([table isKindOfClass:[NSTableView class]]) {
                i32 row = index;
                void (^b)(void) = ^{
                    id dlg = [table delegate];
                    if ([dlg respondsToSelector:@selector(setSuppressNotify:)])
                        [dlg setSuppressNotify:YES];
                    [table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
                       byExtendingSelection:NO];
                    if ([dlg respondsToSelector:@selector(setSuppressNotify:)])
                        [dlg setSuppressNotify:NO];
                };
                if ([NSThread isMainThread]) b(); else dispatch_async(dispatch_get_main_queue(), b);
            }
        }
    }
    return index;
}

void CocoaBridge::set_listbox_ui_selection(HWND ctrl_hwnd, i32 index) {
    m_listbox_selection[ctrl_hwnd] = index;
}

i32 CocoaBridge::listbox_get_cur_sel(HWND ctrl_hwnd) const {
    auto it = m_listbox_selection.find(ctrl_hwnd);
    if (it == m_listbox_selection.end()) return -1;
    return it->second;
}

std::wstring CocoaBridge::listbox_get_item_text(HWND ctrl_hwnd, i32 index) const {
    auto it = m_listbox_items.find(ctrl_hwnd);
    if (it == m_listbox_items.end()) return {};

    const auto& items = it->second;
    if (index < 0 || index >= static_cast<i32>(items.size())) return {};
    return items[index];
}

// =============================================================================
// ListView (SysListView32) support
// =============================================================================

/// Helper: get the NSTextView inside the NSScrollView for a listview hwnd
static NSTextView* get_listview_textview(NSView* view) {
    return vtr_document_text_view(view);
}

i32 CocoaBridge::listview_insert_item(HWND ctrl_hwnd, const std::wstring& text) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) return -1;

    auto& items = m_listview_items[ctrl_hwnd];
    i32 index = static_cast<i32>(items.size());
    items.push_back(text);

    NSView* view = (__bridge NSView*)it->second;
    NSTextView* tv = get_listview_textview(view);
    if (!tv) return index;

    // Convert wstring to NSString
    NSString* line = [[NSString alloc]
        initWithBytes:text.data()
               length:text.size() * sizeof(wchar_t)
             encoding:NSUTF32LittleEndianStringEncoding];
    if (!line) line = @"";

    NSString* appendStr = (index == 0) ? line : [@"\n" stringByAppendingString:line];

    void (^updateBlock)(void) = ^{
        NSTextStorage* storage = [tv textStorage];
        [storage beginEditing];
        NSAttributedString* attrStr = [[NSAttributedString alloc]
            initWithString:appendStr
                attributes:@{
                    NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11.0 weight:NSFontWeightRegular],
                    NSForegroundColorAttributeName: [NSColor textColor]
                }];
        [storage appendAttributedString:attrStr];
        [storage endEditing];
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), updateBlock);
    }

    VITREO_TRACE( "[cocoa] listview_insert_item: hwnd=0x%x item=%d \"%s\"\n",
                 ctrl_hwnd, index, [line UTF8String]);
    return index;
}

bool CocoaBridge::listview_set_item_text(HWND ctrl_hwnd, i32 item_index, const std::wstring& text) {
    auto items_it = m_listview_items.find(ctrl_hwnd);
    if (items_it == m_listview_items.end()) return false;
    auto& items = items_it->second;
    if (item_index < 0 || item_index >= static_cast<i32>(items.size())) return false;

    items[item_index] = text;

    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) return false;

    NSView* view = (__bridge NSView*)it->second;
    NSTextView* tv = get_listview_textview(view);
    if (!tv) return false;

    // Rebuild the entire text content (simple approach for correctness)
    NSMutableString* full = [NSMutableString string];
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) [full appendString:@"\n"];
        NSString* line = [[NSString alloc]
            initWithBytes:items[i].data()
                   length:items[i].size() * sizeof(wchar_t)
                 encoding:NSUTF32LittleEndianStringEncoding];
        if (line) [full appendString:line];
    }

    void (^updateBlock)(void) = ^{
        NSTextStorage* storage = [tv textStorage];
        [storage beginEditing];
        NSAttributedString* attrStr = [[NSAttributedString alloc]
            initWithString:full
                attributes:@{
                    NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11.0 weight:NSFontWeightRegular],
                    NSForegroundColorAttributeName: [NSColor textColor]
                }];
        [storage setAttributedString:attrStr];
        [storage endEditing];
    };

    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), updateBlock);
    }

    return true;
}

i32 CocoaBridge::listview_get_item_count(HWND ctrl_hwnd) const {
    auto it = m_listview_items.find(ctrl_hwnd);
    if (it == m_listview_items.end()) return 0;
    return static_cast<i32>(it->second.size());
}

void CocoaBridge::listview_ensure_visible(HWND ctrl_hwnd, i32 item_index) {
    auto it = m_controls.find(ctrl_hwnd);
    if (it == m_controls.end()) return;

    NSView* view = (__bridge NSView*)it->second;
    NSTextView* tv = get_listview_textview(view);
    if (!tv) return;

    void (^scrollBlock)(void) = ^{
        NSUInteger len = [[tv string] length];
        if (len > 0) {
            [tv scrollRangeToVisible:NSMakeRange(len - 1, 1)];
        }
    };

    if ([NSThread isMainThread]) {
        scrollBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), scrollBlock);
    }
}

// =============================================================================
// TreeView (SysTreeView32) support
// =============================================================================

u64 CocoaBridge::treeview_insert_item(HWND ctrl_hwnd, u64 h_parent,
                                      const std::wstring& text, u64 lparam,
                                      bool has_children) {
    __block u64 result = 0;
    void (^work)(void) = ^{
        auto it = m_treeviews.find(ctrl_hwnd);
        if (it == m_treeviews.end()) return;
        TreeViewData& tv = it->second;

        // TVI_ROOT ((HTREEITEM)-0x10000) or 0 → root-level item.
        const bool is_root = (h_parent == 0) ||
                             (h_parent >= 0xFFFFFFFFFFFF0000ULL) ||
                             (h_parent == 0xFFFF0000ULL);
        u64 parent_item = 0;
        if (!is_root && tv.items.find(h_parent) != tv.items.end()) {
            parent_item = h_parent;
        }

        u64 hitem = tv.next_hitem++;
        TreeItemData item;
        item.parent = parent_item;
        item.lparam = lparam;
        item.has_children = has_children;
        item.text   = text;
        tv.items[hitem] = item;
        if (parent_item == 0) tv.roots.push_back(hitem);
        else tv.items[parent_item].children.push_back(hitem);

        VtrTreeController* ctl = (__bridge VtrTreeController*)tv.controller;
        NSOutlineView*     ov  = (__bridge NSOutlineView*)tv.outline;
        if (ctl) {
            VtrTreeNode* node = [[[VtrTreeNode alloc] init] autorelease];
            node.hItem = hitem;
            node.text  = strip_win_accelerator(to_nsstring(text));
            node.hasChildren = has_children ? YES : NO;
            [ctl.nodeByItem setObject:node forKey:@(hitem)];
            if (parent_item == 0) {
                [ctl.roots addObject:node];
            } else {
                VtrTreeNode* p = [ctl.nodeByItem objectForKey:@(parent_item)];
                if (p) [p.children addObject:node]; else [ctl.roots addObject:node];
            }
            if (ov) {
                [ov reloadData];
                // Expand only the directly-affected root so the tree is
                // navigable without the O(n^2) full-tree expansion storm that
                // an expandItem:nil expandChildren:YES per insert would cause.
                if (parent_item == 0) {
                    ctl.suppressNotify = YES;
                    [ov expandItem:node];
                    ctl.suppressNotify = NO;
                }
            }
        }
        result = hitem;
    };
    if ([NSThread isMainThread]) work(); else vtr_run_on_main_sync(work);
    return result;
}

bool CocoaBridge::treeview_get_item(HWND ctrl_hwnd, u64 h_item,
                                    std::wstring* text_out, u64* lparam_out) const {
    auto it = m_treeviews.find(ctrl_hwnd);
    if (it == m_treeviews.end()) return false;
    auto iit = it->second.items.find(h_item);
    if (iit == it->second.items.end()) return false;
    if (text_out)   *text_out   = iit->second.text;
    if (lparam_out) *lparam_out = iit->second.lparam;
    return true;
}

bool CocoaBridge::treeview_delete_item(HWND ctrl_hwnd, u64 h_item) {
    __block bool result = false;
    void (^work)(void) = ^{
        auto tree_it = m_treeviews.find(ctrl_hwnd);
        if (tree_it == m_treeviews.end()) return;
        TreeViewData& tree = tree_it->second;
        VtrTreeController* controller =
            (__bridge VtrTreeController*)tree.controller;
        NSOutlineView* outline = (__bridge NSOutlineView*)tree.outline;
        const bool delete_all = h_item == 0xFFFF0000ULL ||
                                h_item >= 0xFFFFFFFFFFFF0000ULL;

        if (delete_all) {
            tree.items.clear();
            tree.roots.clear();
            tree.selected = 0;
            if (controller) {
                [controller.roots removeAllObjects];
                [controller.nodeByItem removeAllObjects];
            }
            if (outline) [outline reloadData];
            result = true;
            return;
        }

        auto item_it = tree.items.find(h_item);
        if (item_it == tree.items.end()) return;
        const u64 parent = item_it->second.parent;
        VtrTreeNode* deleted_node = controller
            ? [controller.nodeByItem objectForKey:@(h_item)]
            : nil;
        std::vector<u64> pending{h_item};
        while (!pending.empty()) {
            const u64 current = pending.back();
            pending.pop_back();
            auto current_it = tree.items.find(current);
            if (current_it == tree.items.end()) continue;
            pending.insert(pending.end(), current_it->second.children.begin(),
                           current_it->second.children.end());
            if (tree.selected == current) tree.selected = 0;
            if (controller) [controller.nodeByItem removeObjectForKey:@(current)];
            tree.items.erase(current_it);
        }

        auto& siblings = parent == 0 ? tree.roots : tree.items[parent].children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), h_item), siblings.end());
        if (controller) {
            NSMutableArray<VtrTreeNode*>* nodes = parent == 0
                ? controller.roots
                : [controller.nodeByItem objectForKey:@(parent)].children;
            if (deleted_node && nodes) [nodes removeObjectIdenticalTo:deleted_node];
        }
        if (outline) [outline reloadData];
        result = true;
    };
    if ([NSThread isMainThread]) work(); else vtr_run_on_main_sync(work);
    return result;
}

u64 CocoaBridge::treeview_get_selection(HWND ctrl_hwnd) const {
    auto it = m_treeviews.find(ctrl_hwnd);
    return (it == m_treeviews.end()) ? 0 : it->second.selected;
}

void CocoaBridge::treeview_select_item(HWND ctrl_hwnd, u64 h_item) {
    void (^work)(void) = ^{
        auto it = m_treeviews.find(ctrl_hwnd);
        if (it == m_treeviews.end()) return;
        it->second.selected = h_item;
        VtrTreeController* ctl = (__bridge VtrTreeController*)it->second.controller;
        NSOutlineView*     ov  = (__bridge NSOutlineView*)it->second.outline;
        if (ctl && ov) {
            VtrTreeNode* node = [ctl.nodeByItem objectForKey:@(h_item)];
            if (node) {
                ctl.suppressNotify = YES;
                NSInteger row = [ov rowForItem:node];
                if (row >= 0) {
                    [ov selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
                        byExtendingSelection:NO];
                }
                ctl.suppressNotify = NO;
            }
        }
    };
    if ([NSThread isMainThread]) work(); else vtr_run_on_main_sync(work);
}

bool CocoaBridge::treeview_notify_target(HWND ctrl_hwnd, HWND* parent_out, u32* id_out) const {
    auto it = m_treeviews.find(ctrl_hwnd);
    if (it == m_treeviews.end()) return false;
    if (parent_out) *parent_out = it->second.parent;
    if (id_out)     *id_out     = it->second.ctrl_id;
    return true;
}

bool CocoaBridge::pop_tree_notify(TreeNotify& out) {
    // Cheap fast path: this is called from the guest GetMessage loop on every
    // iteration, so it must NOT do a main-thread roundtrip. The atomic gate
    // makes the common (no pending click) case a single relaxed load.
    if (!m_has_tree_notify.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lk(m_tree_notify_mutex);
    if (m_tree_notifies.empty()) {
        m_has_tree_notify.store(false, std::memory_order_release);
        return false;
    }
    out = m_tree_notifies.front();
    m_tree_notifies.pop_front();
    if (m_tree_notifies.empty()) m_has_tree_notify.store(false, std::memory_order_release);
    return true;
}

} // namespace vitreo::windowing
