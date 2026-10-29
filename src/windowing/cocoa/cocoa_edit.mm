// SPDX-License-Identifier: LGPL-2.1-only
/// @file cocoa_edit.mm
/// @brief Layout, selection and scroll queries on the native views behind
/// EDIT controls (NSTextField / NSSecureTextField for single-line, an
/// NSScrollView + NSTextView for ES_MULTILINE). user32's EDIT class
/// procedure (control_edit.cpp) owns the Windows-visible text and state and
/// asks these for what only the layout knows. Indices are NSString (UTF-16)
/// indices of the displayed string. Nothing here moves keyboard focus:
/// EM_SETSEL on Windows never does.

#import <Cocoa/Cocoa.h>
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vitreo::windowing {

namespace {

NSView* control_view(const std::unordered_map<HWND, void*>& controls, HWND hwnd) {
    const auto it = controls.find(hwnd);
    return it == controls.end() || it->second == nullptr ? nil : (__bridge NSView*)it->second;
}

/// The live text object: an NSTextView, or a text field's field editor while
/// the field is being edited (nil otherwise).
NSText* live_text(NSView* view) {
    if (NSTextView* tv = vtr_document_text_view(view)) return tv;
    if ([view isKindOfClass:[NSTextField class]]) {
        return [(NSTextField*)view currentEditor];
    }
    return nil;
}

/// Visual line fragments of a text view, as character ranges.
std::vector<NSRange> line_fragments(NSTextView* tv) {
    std::vector<NSRange> lines;
    NSLayoutManager* lm = [tv layoutManager];
    NSTextContainer* tc = [tv textContainer];
    [lm ensureLayoutForTextContainer:tc];
    const NSUInteger glyphs = [lm numberOfGlyphs];
    NSUInteger g = 0;
    while (g < glyphs) {
        NSRange fragment_glyphs{};
        [lm lineFragmentRectForGlyphAtIndex:g effectiveRange:&fragment_glyphs];
        lines.push_back([lm characterRangeForGlyphRange:fragment_glyphs actualGlyphRange:nullptr]);
        g = NSMaxRange(fragment_glyphs);
        if (fragment_glyphs.length == 0) break;
    }
    if (lines.empty() || [lm extraLineFragmentTextContainer] != nil) {
        lines.push_back(NSMakeRange([[tv string] length], 0));
    }
    return lines;
}

}  // namespace

bool CocoaBridge::edit_get_selection(HWND hwnd, u32& start, u32& end) const {
    __block bool live = false;
    __block NSRange range{};
    vtr_run_on_main_sync(^{
        NSText* text = live_text(control_view(m_controls, hwnd));
        if (text == nil) return;
        range = [text selectedRange];
        live = true;
    });
    if (!live) return false;
    start = static_cast<u32>(range.location);
    end = static_cast<u32>(NSMaxRange(range));
    return true;
}

bool CocoaBridge::edit_set_selection(HWND hwnd, u32 start, u32 end) {
    __block bool applied = false;
    vtr_run_on_main_sync(^{
        NSText* text = live_text(control_view(m_controls, hwnd));
        if (text == nil) return;
        const NSUInteger len = [[text string] length];
        const NSUInteger a = std::min<NSUInteger>(start, len);
        const NSUInteger b = std::min<NSUInteger>(end, len);
        [text setSelectedRange:NSMakeRange(std::min(a, b), (a > b ? a - b : b - a))];
        applied = true;
    });
    return applied;
}

bool CocoaBridge::edit_scroll_caret(HWND hwnd) {
    __block bool ok = false;
    vtr_run_on_main_sync(^{
        NSText* text = live_text(control_view(m_controls, hwnd));
        if (text == nil) return;
        [text scrollRangeToVisible:[text selectedRange]];
        ok = true;
    });
    return ok;
}

void CocoaBridge::edit_set_read_only(HWND hwnd, bool read_only) {
    vtr_run_on_main_sync(^{
        NSView* view = control_view(m_controls, hwnd);
        if (NSTextView* tv = vtr_document_text_view(view)) {
            [tv setEditable:!read_only];
        } else if ([view isKindOfClass:[NSTextField class]]) {
            [(NSTextField*)view setEditable:!read_only];
        }
    });
}

void CocoaBridge::edit_set_cue_banner(HWND hwnd, const std::wstring& cue) {
    NSString* text = to_nsstring(cue);  // +1 or a literal
    vtr_run_on_main_sync(^{
        NSView* view = control_view(m_controls, hwnd);
        if ([view isKindOfClass:[NSTextField class]]) {
            [(NSTextField*)view setPlaceholderString:text];
        }
    });
    [text release];
}

std::vector<u32> CocoaBridge::edit_visual_line_starts(HWND hwnd) const {
    __block std::vector<u32> starts;
    vtr_run_on_main_sync(^{
        NSTextView* tv = vtr_document_text_view(control_view(m_controls, hwnd));
        if (tv == nil) return;
        for (const NSRange& r : line_fragments(tv)) starts.push_back(static_cast<u32>(r.location));
    });
    return starts;
}

i32 CocoaBridge::edit_first_visible_line(HWND hwnd) const {
    __block i32 line = 0;
    vtr_run_on_main_sync(^{
        NSTextView* tv = vtr_document_text_view(control_view(m_controls, hwnd));
        if (tv == nil) return;
        const NSRect visible = [tv visibleRect];
        const auto lines = line_fragments(tv);
        NSLayoutManager* lm = [tv layoutManager];
        for (usize i = 0; i < lines.size(); ++i) {
            if (lines[i].length == 0) break;
            const NSRange glyphs = [lm glyphRangeForCharacterRange:lines[i] actualCharacterRange:nullptr];
            const NSRect r = [lm lineFragmentRectForGlyphAtIndex:glyphs.location effectiveRange:nullptr];
            if (NSMaxY(r) > NSMinY(visible) + 0.5) {
                line = static_cast<i32>(i);
                return;
            }
        }
    });
    return line;
}

i32 CocoaBridge::edit_scroll_lines(HWND hwnd, i32 lines, i32 columns, bool page) {
    __block i32 moved = 0;
    vtr_run_on_main_sync(^{
        NSView* view = control_view(m_controls, hwnd);
        NSTextView* tv = vtr_document_text_view(view);
        if (tv == nil) return;
        NSScrollView* scroll = (NSScrollView*)view;
        NSFont* font = [tv font] ?: [NSFont systemFontOfSize:11.0];
        const CGFloat line_h = [[tv layoutManager] defaultLineHeightForFont:font];
        const CGFloat col_w = [font maximumAdvancement].width > 0 ? [font maximumAdvancement].width : 7.0;
        NSClipView* clip = [scroll contentView];
        NSPoint origin = [clip bounds].origin;
        const NSRect doc = [[scroll documentView] frame];
        const CGFloat max_y = std::max<CGFloat>(0, NSHeight(doc) - NSHeight([clip bounds]));
        const CGFloat max_x = std::max<CGFloat>(0, NSWidth(doc) - NSWidth([clip bounds]));
        const CGFloat before = origin.y;
        // A page is the number of whole lines the view shows (at least one).
        const CGFloat step = page
            ? std::max<CGFloat>(1, std::floor(NSHeight([clip bounds]) / line_h)) * line_h
            : line_h;
        origin.y = std::clamp<CGFloat>(origin.y + lines * step, 0, max_y);
        origin.x = std::clamp<CGFloat>(origin.x + columns * col_w, 0, max_x);
        [clip scrollToPoint:origin];
        [scroll reflectScrolledClipView:clip];
        moved = static_cast<i32>(std::lround((origin.y - before) / line_h));
    });
    return moved;
}

bool CocoaBridge::edit_pos_from_char(HWND hwnd, u32 index, i32& x, i32& y) const {
    __block bool ok = false;
    __block NSPoint p{};
    vtr_run_on_main_sync(^{
        NSView* view = control_view(m_controls, hwnd);
        if (NSTextView* tv = vtr_document_text_view(view)) {
            NSLayoutManager* lm = [tv layoutManager];
            [lm ensureLayoutForTextContainer:[tv textContainer]];
            if (index >= [[tv string] length]) return;
            const NSUInteger glyph = [lm glyphIndexForCharacterAtIndex:index];
            const NSRect frag = [lm lineFragmentRectForGlyphAtIndex:glyph effectiveRange:nullptr];
            const NSPoint loc = [lm locationForGlyphAtIndex:glyph];
            const NSPoint origin = [tv textContainerOrigin];
            NSPoint in_tv = NSMakePoint(frag.origin.x + loc.x + origin.x, frag.origin.y + origin.y);
            p = [view convertPoint:in_tv fromView:tv];
            if (![view isFlipped]) p.y = NSHeight([view bounds]) - p.y;
            ok = true;
        } else if ([view isKindOfClass:[NSTextField class]]) {
            NSTextField* field = (NSTextField*)view;
            NSString* s = [field stringValue];
            if (index >= [s length]) return;
            NSDictionary* attrs = @{NSFontAttributeName: [field font] ?: [NSFont systemFontOfSize:11.0]};
            const NSSize prefix = [[s substringToIndex:index] sizeWithAttributes:attrs];
            p = NSMakePoint(prefix.width + 2.0, 1.0);
            ok = true;
        }
    });
    if (!ok) return false;
    x = static_cast<i32>(std::lround(p.x));
    y = static_cast<i32>(std::lround(p.y));
    return true;
}

bool CocoaBridge::edit_char_from_pos(HWND hwnd, i32 x, i32 y, u32& index) const {
    __block bool ok = false;
    __block NSUInteger found = 0;
    vtr_run_on_main_sync(^{
        NSView* view = control_view(m_controls, hwnd);
        if (NSTextView* tv = vtr_document_text_view(view)) {
            NSPoint p = NSMakePoint(x, y);
            if (![view isFlipped]) p.y = NSHeight([view bounds]) - p.y;
            p = [tv convertPoint:p fromView:view];
            const NSPoint origin = [tv textContainerOrigin];
            p.x -= origin.x;
            p.y -= origin.y;
            found = [[tv layoutManager] characterIndexForPoint:p
                                               inTextContainer:[tv textContainer]
                      fractionOfDistanceBetweenInsertionPoints:nullptr];
            ok = true;
        } else if ([view isKindOfClass:[NSTextField class]]) {
            NSTextField* field = (NSTextField*)view;
            NSString* s = [field stringValue];
            NSDictionary* attrs = @{NSFontAttributeName: [field font] ?: [NSFont systemFontOfSize:11.0]};
            NSUInteger i = 0;
            for (; i < [s length]; ++i) {
                const CGFloat w = [[s substringToIndex:i + 1] sizeWithAttributes:attrs].width;
                if (w + 2.0 > x) break;
            }
            found = i;
            ok = true;
        }
    });
    if (!ok) return false;
    index = static_cast<u32>(found);
    return true;
}

bool CocoaBridge::edit_get_rect(HWND hwnd, Rect& rect) const {
    __block bool ok = false;
    __block NSRect r{};
    vtr_run_on_main_sync(^{
        NSView* view = control_view(m_controls, hwnd);
        if (view == nil) return;
        r = [view bounds];
        if (NSTextView* tv = vtr_document_text_view(view)) {
            const NSSize inset = [tv textContainerInset];
            const CGFloat pad = [[tv textContainer] lineFragmentPadding];
            r = NSInsetRect([((NSScrollView*)view) documentVisibleRect], 0, 0);
            r.origin = NSMakePoint(inset.width + pad, inset.height);
            r.size.width -= 2 * (inset.width + pad);
            r.size.height -= 2 * inset.height;
        }
        ok = true;
    });
    if (!ok) return false;
    rect.left = static_cast<i32>(std::lround(NSMinX(r)));
    rect.top = static_cast<i32>(std::lround(NSMinY(r)));
    rect.right = static_cast<i32>(std::lround(NSMaxX(r)));
    rect.bottom = static_cast<i32>(std::lround(NSMaxY(r)));
    return true;
}

void CocoaBridge::edit_set_margins(HWND hwnd, i32 left, i32 right) {
    vtr_run_on_main_sync(^{
        NSTextView* tv = vtr_document_text_view(control_view(m_controls, hwnd));
        if (tv == nil) return;
        // Asymmetric margins: the container keeps the left one as padding and
        // the text view's inset; a right margin narrows the container.
        [[tv textContainer] setLineFragmentPadding:0];
        [tv setTextContainerInset:NSMakeSize(std::max(0, left), 0)];
        NSTextContainer* tc = [tv textContainer];
        if (right > 0) {
            [tc setWidthTracksTextView:NO];
            [tc setContainerSize:NSMakeSize(std::max<CGFloat>(0, NSWidth([tv bounds]) - left - right),
                                            CGFLOAT_MAX)];
        } else {
            [tc setWidthTracksTextView:YES];
        }
    });
}

void CocoaBridge::edit_set_tab_stops(HWND hwnd, const std::vector<f64>& points) {
    std::vector<f64> copy = points;
    vtr_run_on_main_sync(^{
        NSTextView* tv = vtr_document_text_view(control_view(m_controls, hwnd));
        if (tv == nil) return;
        NSMutableParagraphStyle* para = [[[NSMutableParagraphStyle alloc] init] autorelease];
        NSMutableArray* stops = [NSMutableArray array];
        if (copy.size() == 1) {
            // One value: stops every that many points.
            [para setDefaultTabInterval:copy[0]];
        } else {
            for (f64 p : copy) {
                [stops addObject:[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                                                  location:p
                                                                   options:@{}] autorelease]];
            }
        }
        [para setTabStops:stops];
        [tv setDefaultParagraphStyle:para];
        NSTextStorage* storage = [tv textStorage];
        [storage addAttribute:NSParagraphStyleAttributeName value:para
                        range:NSMakeRange(0, [storage length])];
    });
}

}  // namespace vitreo::windowing
