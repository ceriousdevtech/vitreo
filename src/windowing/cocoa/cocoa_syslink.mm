// SPDX-License-Identifier: LGPL-2.1-only
/// @file cocoa_syslink.mm
/// @brief SysLink (WC_LINK) renderer. user32 owns the link items and their
/// LIS_* state (control_syslink.cpp); this view draws them and answers
/// LM_HITTEST / LM_GETIDEALSIZE from the layout it draws. Compiled without
/// ARC.

#import <Cocoa/Cocoa.h>
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace vitreo;
using namespace vitreo::windowing;

@implementation VtrSysLinkView {
    NSTextStorage* _storage;
    NSLayoutManager* _layout;
    NSTextContainer* _container;
    NSFont* _font;
    NSString* _display;
    std::vector<uint32_t> _starts;
    std::vector<uint32_t> _lengths;
    std::vector<uint32_t> _states;
    int32_t _mnemonic;
    uint32_t _style;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _storage = [[NSTextStorage alloc] init];
        _layout = [[NSLayoutManager alloc] init];
        _container = [[NSTextContainer alloc]
            initWithContainerSize:NSMakeSize(NSWidth(frame), CGFLOAT_MAX)];
        [_container setLineFragmentPadding:0];
        [_layout addTextContainer:_container];
        [_storage addLayoutManager:_layout];
        // The dense default GUI font the Win32 layout was sized for (same
        // default the bridge applies for a NULL WM_SETFONT).
        _font = [[NSFont systemFontOfSize:11.0] retain];
        _display = [@"" retain];
        _mnemonic = -1;
        _style = 0;
    }
    return self;
}

- (void)dealloc {
    [_storage release];
    [_layout release];
    [_container release];
    [_font release];
    [_display release];
    [super dealloc];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
// Input is delivered to the HWND as WM_* by the event pump; the SysLink
// class procedure in user32 handles it. Do not forward to the superview.
- (void)mouseDown:(NSEvent*)event { (void)event; }
- (void)mouseUp:(NSEvent*)event { (void)event; }

- (NSAttributedString*)attributedText {
    NSMutableParagraphStyle* para = [[[NSMutableParagraphStyle alloc] init] autorelease];
    [para setAlignment:(_style & syslink::LWS_RIGHT) ? NSTextAlignmentRight
                                                      : NSTextAlignmentLeft];
    [para setLineBreakMode:NSLineBreakByWordWrapping];
    NSDictionary* base = @{
        NSFontAttributeName: _font,
        NSForegroundColorAttributeName: [NSColor textColor],
        NSParagraphStyleAttributeName: para,
    };
    NSMutableAttributedString* text =
        [[[NSMutableAttributedString alloc] initWithString:_display
                                                attributes:base] autorelease];
    const NSUInteger total = [_display length];
    for (size_t i = 0; i < _starts.size(); ++i) {
        const NSRange r = NSMakeRange(_starts[i], _lengths[i]);
        if (r.length == 0 || NSMaxRange(r) > total) continue;
        const bool enabled = (_states[i] & syslink::LIS_ENABLED) != 0;
        [text addAttribute:NSForegroundColorAttributeName
                     value:enabled ? [NSColor linkColor] : [NSColor disabledControlTextColor]
                     range:r];
        if (enabled) {
            [text addAttribute:NSUnderlineStyleAttributeName
                         value:@(NSUnderlineStyleSingle)
                         range:r];
        }
    }
    if (_mnemonic >= 0 && static_cast<NSUInteger>(_mnemonic) < total) {
        [text addAttribute:NSUnderlineStyleAttributeName
                     value:@(NSUnderlineStyleSingle)
                     range:NSMakeRange(static_cast<NSUInteger>(_mnemonic), 1)];
    }
    return text;
}

- (void)rebuild {
    [_container setContainerSize:NSMakeSize(NSWidth([self bounds]), CGFLOAT_MAX)];
    [_storage setAttributedString:[self attributedText]];
    [self setNeedsDisplay:YES];
}

- (void)setDisplay:(NSString*)display
            starts:(const uint32_t*)starts
           lengths:(const uint32_t*)lengths
            states:(const uint32_t*)states
             count:(size_t)count
          mnemonic:(int32_t)mnemonic
             style:(uint32_t)style {
    [_display release];
    _display = [(display ?: @"") copy];
    _starts.assign(starts, starts + count);
    _lengths.assign(lengths, lengths + count);
    _states.assign(states, states + count);
    _mnemonic = mnemonic;
    _style = style;
    [self rebuild];
}

- (void)setLinkFont:(NSFont*)font {
    if (font == nil) return;
    [_font release];
    _font = [font retain];
    [self rebuild];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [_container setContainerSize:NSMakeSize(size.width, CGFLOAT_MAX)];
    [self setNeedsDisplay:YES];
}

- (NSRange)glyphRangeOfItem:(size_t)index {
    return [_layout glyphRangeForCharacterRange:NSMakeRange(_starts[index], _lengths[index])
                           actualCharacterRange:nullptr];
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRange glyphs = [_layout glyphRangeForTextContainer:_container];
    [_layout drawGlyphsForGlyphRange:glyphs atPoint:NSZeroPoint];
    // LIS_FOCUSED: the keyboard-focus rectangle around that link.
    for (size_t i = 0; i < _states.size(); ++i) {
        if (!(_states[i] & syslink::LIS_FOCUSED) || _lengths[i] == 0) continue;
        const NSRect r = NSInsetRect(
            [_layout boundingRectForGlyphRange:[self glyphRangeOfItem:i]
                               inTextContainer:_container], -0.5, -0.5);
        NSBezierPath* path = [NSBezierPath bezierPathWithRect:r];
        const CGFloat dash[] = {1.0, 1.0};
        [path setLineDash:dash count:2 phase:0];
        [path setLineWidth:1.0];
        [[NSColor textColor] setStroke];
        [path stroke];
    }
}

- (int32_t)itemAtPoint:(NSPoint)point {
    if (_starts.empty() || [_display length] == 0) return -1;
    CGFloat fraction = 0;
    const NSUInteger glyph = [_layout glyphIndexForPoint:point
                                         inTextContainer:_container
                          fractionOfDistanceThroughGlyph:&fraction];
    const NSRect box = [_layout boundingRectForGlyphRange:NSMakeRange(glyph, 1)
                                          inTextContainer:_container];
    if (!NSPointInRect(point, box)) return -1;
    const NSUInteger ch = [_layout characterIndexForGlyphAtIndex:glyph];
    for (size_t i = 0; i < _starts.size(); ++i) {
        if (ch >= _starts[i] && ch < static_cast<NSUInteger>(_starts[i]) + _lengths[i]) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

- (NSSize)idealSizeForWidth:(CGFloat)width {
    NSTextStorage* storage = [[NSTextStorage alloc]
        initWithAttributedString:[self attributedText]];
    NSLayoutManager* layout = [[NSLayoutManager alloc] init];
    NSTextContainer* container = [[NSTextContainer alloc]
        initWithContainerSize:NSMakeSize(width > 0 ? width : CGFLOAT_MAX, CGFLOAT_MAX)];
    [container setLineFragmentPadding:0];
    [layout addTextContainer:container];
    [storage addLayoutManager:layout];
    [layout ensureLayoutForTextContainer:container];
    NSRect used = [layout usedRectForTextContainer:container];
    if ([_display length] == 0) {
        // An empty control still reports one line of the font.
        used.size.height = [layout defaultLineHeightForFont:_font];
        used.size.width = 0;
    }
    [container release];
    [layout release];
    [storage release];
    return NSMakeSize(std::ceil(NSWidth(used)), std::ceil(NSHeight(used)));
}

@end

void vtr_syslink_apply(VtrSysLinkView* view, const syslink::Markup& markup,
                       const std::vector<u32>& states, u32 style) {
    if (view == nil) return;
    // Markup offsets index the wstring (UTF-32 here); NSString indexes UTF-16.
    std::vector<uint32_t> utf16_at(markup.display.size() + 1, 0);
    for (usize i = 0; i < markup.display.size(); ++i) {
        utf16_at[i + 1] = utf16_at[i] + (static_cast<u32>(markup.display[i]) > 0xFFFFu ? 2u : 1u);
    }
    std::vector<uint32_t> starts;
    std::vector<uint32_t> lengths;
    for (const auto& item : markup.items) {
        const usize end = std::min<usize>(item.start + item.length, markup.display.size());
        const usize begin = std::min<usize>(item.start, end);
        starts.push_back(utf16_at[begin]);
        lengths.push_back(utf16_at[end] - utf16_at[begin]);
    }
    std::vector<uint32_t> item_states(states.begin(), states.end());
    item_states.resize(starts.size(), syslink::LIS_ENABLED);
    const i32 mnemonic = markup.mnemonic >= 0 &&
            static_cast<usize>(markup.mnemonic) < markup.display.size()
        ? static_cast<i32>(utf16_at[static_cast<usize>(markup.mnemonic)])
        : -1;
    // to_nsstring returns +1 (alloc) or a literal.
    NSString* display = to_nsstring(markup.display);
    [view setDisplay:display
              starts:starts.data()
             lengths:lengths.data()
              states:item_states.data()
               count:starts.size()
            mnemonic:mnemonic
               style:style];
    [display release];
}

namespace vitreo::windowing {

namespace {

VtrSysLinkView* syslink_view(const std::unordered_map<HWND, void*>& controls, HWND hwnd) {
    const auto it = controls.find(hwnd);
    if (it == controls.end() || it->second == nullptr) return nil;
    NSView* view = (__bridge NSView*)it->second;
    return [view isKindOfClass:[VtrSysLinkView class]] ? (VtrSysLinkView*)view : nil;
}

}  // namespace

void CocoaBridge::syslink_set_content(HWND hwnd, const syslink::Markup& markup,
                                      const std::vector<u32>& states, u32 style) {
    vtr_run_on_main_sync(^{
        vtr_syslink_apply(syslink_view(m_controls, hwnd), markup, states, style);
    });
}

i32 CocoaBridge::syslink_hit_test(HWND hwnd, i32 x, i32 y) const {
    __block i32 index = -1;
    vtr_run_on_main_sync(^{
        VtrSysLinkView* view = syslink_view(m_controls, hwnd);
        if (view != nil) index = [view itemAtPoint:NSMakePoint(x, y)];
    });
    return index;
}

bool CocoaBridge::syslink_ideal_size(HWND hwnd, i32 max_width, i32& cx, i32& cy) const {
    __block bool found = false;
    __block NSSize size = NSZeroSize;
    vtr_run_on_main_sync(^{
        VtrSysLinkView* view = syslink_view(m_controls, hwnd);
        if (view == nil) return;
        size = [view idealSizeForWidth:static_cast<CGFloat>(max_width)];
        found = true;
    });
    if (!found) return false;
    cx = static_cast<i32>(size.width);
    cy = static_cast<i32>(size.height);
    return true;
}

}  // namespace vitreo::windowing
