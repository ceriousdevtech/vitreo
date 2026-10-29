// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_strings.mm
/// @brief Guest text to NSString and back, and Win32 accelerator markers.
///
/// The string helpers objc_helpers.h declares: wide-string conversion,
/// accelerator stripping and underlining, and the text view inside a
/// scroll-view-backed control.
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
// Helper: wstring → NSString
// =============================================================================

NSString* to_nsstring(const std::wstring& ws) {
    if (ws.empty()) return @"";
    return [[NSString alloc] initWithBytes:ws.data()
                                    length:ws.size() * sizeof(wchar_t)
                                  encoding:NSUTF32LittleEndianStringEncoding];
}

std::wstring from_nsstring(NSString* s) {
    if (!s || [s length] == 0) return {};
    // `dataUsingEncoding:NSUTF32LittleEndianStringEncoding` prepends U+FEFF.
    // `getBytes:…` does not. Strip a BOM or trailing NUL defensively so
    // GetWindowText's TCHAR count matches NSString.length.
    const NSUInteger utf16_len = [s length];
    std::vector<wchar_t> buf(utf16_len + 2, 0);
    NSUInteger used = 0;
    const BOOL ok = [s getBytes:buf.data()
                      maxLength:buf.size() * sizeof(wchar_t)
                     usedLength:&used
                       encoding:NSUTF32LittleEndianStringEncoding
                        options:0
                          range:NSMakeRange(0, utf16_len)
                 remainingRange:nullptr];
    if (!ok || used < sizeof(wchar_t)) {
        return {};
    }
    std::size_t count = used / sizeof(wchar_t);
    while (count > 0 && buf[count - 1] == 0) {
        --count;
    }
    std::size_t start = 0;
    if (count > 0 && static_cast<char32_t>(buf[0]) == 0xFEFF) {
        start = 1;
    }
    return std::wstring(buf.data() + start, count - start);
}

/// Strip Windows accelerator-key prefixes for display and extract the shortcut
/// character (if any) for use as a macOS keyEquivalent.
///
/// Rules: '&&' → '&'  (escaped literal ampersand, no accelerator)
///        '&X'  → 'X', accel = lowercase(X)
NSString* strip_win_accelerator(NSString* s, unichar* out_accel,
                                       NSRange* out_accel_range) {
    if (out_accel) *out_accel = 0;
    if (out_accel_range) *out_accel_range = NSMakeRange(NSNotFound, 0);
    if ([s rangeOfString:@"&"].location == NSNotFound) return s;
    NSMutableString* result = [NSMutableString stringWithCapacity:s.length];
    NSUInteger i = 0, len = s.length;
    while (i < len) {
        unichar c = [s characterAtIndex:i];
        if (c == '&' && i + 1 < len) {
            unichar next = [s characterAtIndex:i + 1];
            [result appendFormat:@"%C", next];
            // First non-escaped '&' → this is the accelerator key
            if (out_accel && *out_accel == 0 && next != '&') {
                *out_accel = (unichar)tolower((int)next);
                if (out_accel_range) {
                    *out_accel_range = NSMakeRange(result.length - 1, 1);
                }
            }
            i += 2;
        } else {
            [result appendFormat:@"%C", c];
            i++;
        }
    }
    return result;
}

void underline_win_accelerator(NSControl* control, NSRange range) {
    if (range.location == NSNotFound) return;

    NSMutableAttributedString* title = nil;
    if ([control isKindOfClass:[NSButton class]]) {
        title = [[(NSButton*)control attributedTitle] mutableCopy];
    } else if ([control isKindOfClass:[NSTextField class]]) {
        title = [[(NSTextField*)control attributedStringValue] mutableCopy];
    }
    if (title == nil || NSMaxRange(range) > title.length) return;

    [title addAttribute:NSUnderlineStyleAttributeName
                  value:@(NSUnderlineStyleSingle)
                  range:range];
    if ([control isKindOfClass:[NSButton class]]) {
        [(NSButton*)control setAttributedTitle:title];
    } else {
        [(NSTextField*)control setAttributedStringValue:title];
    }
}

/// Resolve an NSTextView from a control view when the control is backed by
/// NSScrollView (multiline EDIT, LISTBOX, SysListView32).
NSTextView* vtr_document_text_view(NSView* view) {
    if (![view isKindOfClass:[NSScrollView class]]) {
        return nil;
    }
    NSView* doc = [(NSScrollView*)view documentView];
    if ([doc isKindOfClass:[NSTextView class]]) {
        return (NSTextView*)doc;
    }
    return nil;
}

} // namespace vitreo::windowing
