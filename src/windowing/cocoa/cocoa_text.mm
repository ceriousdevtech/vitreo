// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_text.mm
/// @brief Core Text rendering and measurement for guest-drawn text.
///
/// Win32 text APIs hand back a bitmap or a size; Core Text works in typeset
/// frames and glyph runs. These render into an off-screen buffer the guest
/// can blit, and measure without rendering — GetTextExtentPoint and
/// DrawText's DT_CALCRECT both need a size before anything is drawn.
///
/// Split out of cocoa_bridge.mm. All methods here are declared in
/// cocoa_bridge.hpp, so this file adds no interface of its own.

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

namespace vitreo::windowing {


// Windows face names that macOS does not ship. Core Text silently substitutes
// an unrelated face for an unknown name (Helvetica-like), which turns every
// fixed-pitch console into a proportional one and breaks column alignment.
// (ascent+descent)/em of the Windows face a guest sized its rows for. A
// substitute with a taller line box (Menlo is 1.16 em, Consolas 1.0) spills
// descenders into the next row, so the substitute is scaled to this ratio.
static float vtr_windows_line_box_ratio(const std::u16string& face_name) {
    if (face_name.empty()) return 0.0f;
    NSString* name = [[[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(face_name.data())
                                                    length:face_name.size()] lowercaseString];
    static NSDictionary<NSString*, NSNumber*>* ratios = @{
        @"consolas": @1.0f, @"lucida console": @1.0f, @"cascadia mono": @1.2f, @"cascadia code": @1.2f,
        @"terminal": @1.0f, @"fixedsys": @1.0f, @"courier new": @1.13f, @"courier": @1.13f,
        @"segoe ui": @1.33f, @"tahoma": @1.21f, @"ms shell dlg": @1.15f, @"ms shell dlg 2": @1.21f,
        @"ms sans serif": @1.15f, @"microsoft sans serif": @1.15f, @"calibri": @1.22f, @"arial": @1.15f,
    };
    NSNumber* r = ratios[name];
    return r ? r.floatValue : 0.0f;
}

// Rescale a substituted face so ascent+descent matches the Windows face's
// line box at this em size. Returns the font to use (consumes `font`).
static CTFontRef vtr_fit_substituted_font(CTFontRef font, const std::u16string& face_name, float pt_size) {
    const float want = vtr_windows_line_box_ratio(face_name);
    if (!font || want <= 0.0f || pt_size <= 0.0f) return font;
    const float have = static_cast<float>((CTFontGetAscent(font) + CTFontGetDescent(font)) / pt_size);
    if (have <= want * 1.02f) return font;
    CTFontRef fitted = CTFontCreateCopyWithAttributes(font, pt_size * want / have, nullptr, nullptr);
    if (!fitted) return font;
    CFRelease(font);
    return fitted;
}

static NSString* vtr_host_face_for(const std::u16string& face_name) {
    if (face_name.empty()) return @"Helvetica";
    NSString* name = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(face_name.data())
                                                   length:face_name.size()];
    static NSDictionary<NSString*, NSString*>* map = @{
        @"consolas": @"Menlo", @"lucida console": @"Menlo", @"courier new": @"Courier New",
        @"courier": @"Courier", @"cascadia mono": @"Menlo", @"cascadia code": @"Menlo",
        @"terminal": @"Menlo", @"fixedsys": @"Menlo", @"segoe ui": @"Helvetica Neue",
        @"tahoma": @"Helvetica Neue", @"ms shell dlg": @"Helvetica Neue", @"ms shell dlg 2": @"Helvetica Neue",
        @"ms sans serif": @"Helvetica Neue", @"microsoft sans serif": @"Helvetica Neue",
        @"calibri": @"Helvetica Neue", @"segoe ui symbol": @"Helvetica Neue",
    };
    NSString* mapped = map[[name lowercaseString]];
    if (mapped) return mapped;
    // Unknown to Core Text as well? Fall back explicitly rather than let it pick.
    CTFontDescriptorRef desc = CTFontDescriptorCreateWithNameAndSize((__bridge CFStringRef)name, 12);
    CFStringRef resolved = desc ? (CFStringRef)CTFontDescriptorCopyAttribute(desc, kCTFontNameAttribute) : nullptr;
    const bool known = resolved != nullptr;
    if (resolved) CFRelease(resolved);
    if (desc) CFRelease(desc);
    return known ? name : @"Helvetica";
}

NSFont* vtr_gdi_matching_font(const std::u16string& face_name, float pt_size, u16 weight,
                              bool italic) {
    // The font GDI text output and measurement use for this face (same
    // substitution and line-box fit as render_text_to_buffer / measure_text).
    NSString* name = vtr_host_face_for(face_name);
    CTFontRef font = CTFontCreateWithName((__bridge CFStringRef)name, pt_size, nullptr);
    font = vtr_fit_substituted_font(font, face_name, pt_size);
    CTFontSymbolicTraits traits = 0;
    if (weight >= 700) traits |= kCTFontBoldTrait;
    if (italic) traits |= kCTFontItalicTrait;
    if (font && traits != 0) {
        CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(font, 0.0, nullptr, traits, traits);
        if (styled) {
            CFRelease(font);
            font = styled;
        }
    }
    if (!font) return nil;
    // Without ARC, CFBridgingRelease already autoreleases the +1 CTFont.
    return (NSFont*)CFBridgingRelease(font);
}

// =============================================================================
// render_text_to_buffer — Core Text off-screen text rendering
// =============================================================================

RenderedTextBuffer CocoaBridge::render_text_to_buffer(
    const std::string& utf8_text,
    float pt_size, u16 weight, bool italic,
    u32 text_color, const std::u16string& face_name)
{
    RenderedTextBuffer result;
    if (utf8_text.empty()) return result;

    @autoreleasepool {
        // 1. Create Core Text font
        CTFontSymbolicTraits traits = 0;
        if (weight >= 700) traits |= kCTFontBoldTrait;
        if (italic)        traits |= kCTFontItalicTrait;

        NSString* name = vtr_host_face_for(face_name);
        CTFontRef font = CTFontCreateWithName((__bridge CFStringRef)name, pt_size, nullptr);
        font = vtr_fit_substituted_font(font, face_name, pt_size);
        if (traits != 0) {
            CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(
                font, pt_size, nullptr, traits, traits);
            if (styled) {
                CFRelease(font);
                font = styled;
            }
        }

        // 2. Create attributed string
        NSString* nsText = [[NSString alloc]
            initWithBytes:utf8_text.data()
                   length:utf8_text.size()
                 encoding:NSUTF8StringEncoding];
        if (!nsText) { CFRelease(font); return result; }

        // Decompose COLORREF (0x00BBGGRR) to RGBA components
        CGFloat r = static_cast<CGFloat>((text_color >>  0) & 0xFF) / 255.0;
        CGFloat g = static_cast<CGFloat>((text_color >>  8) & 0xFF) / 255.0;
        CGFloat b = static_cast<CGFloat>((text_color >> 16) & 0xFF) / 255.0;
        NSColor* nsColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];

        NSDictionary* attrs = @{
            (__bridge id)kCTFontAttributeName:            (__bridge id)font,
            (__bridge id)kCTForegroundColorAttributeName: nsColor,
        };
        NSAttributedString* attrStr =
            [[NSAttributedString alloc] initWithString:nsText attributes:attrs];

        // 3. Measure text
        CTLineRef line = CTLineCreateWithAttributedString(
            (__bridge CFAttributedStringRef)attrStr);
        CGFloat ascent = 0, descent = 0, leading = 0;
        double advance = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);

        i32 buf_w = static_cast<i32>(std::ceil(advance)) + 2; // small pad
        i32 buf_h = static_cast<i32>(std::ceil(ascent + descent + leading)) + 2;
        if (buf_w <= 0 || buf_h <= 0) {
            CFRelease(line);
            CFRelease(font);
            return result;
        }

        // 4. Create bitmap context (RGBA, premultiplied alpha)
        size_t row_bytes = static_cast<size_t>(buf_w) * 4;
        std::vector<u8> pixels(row_bytes * static_cast<size_t>(buf_h), 0);

        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(
            pixels.data(), buf_w, buf_h, 8, row_bytes,
            cs,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);

        if (!ctx) {
            CFRelease(line);
            CFRelease(font);
            return result;
        }

        // 5. Draw text  (Core Graphics Y=0 is bottom-left)
        //
        // Disable anti-aliasing / font smoothing.  This buffer is composited
        // into a per-hwnd GDI framebuffer that Vitreo presents 1:1 in Win32
        // pixels and then upscales to Retina with nearest-neighbor filtering.
        // Grayscale-AA glyph edges (partial alpha) survive that upscale as soft,
        // blurry blocks, whereas aliased 1-bit glyphs stay razor-sharp and match
        // the crisp bitmap card faces — the classic Windows GDI look.
        CGContextSetShouldAntialias(ctx, false);
        CGContextSetAllowsAntialiasing(ctx, false);
        CGContextSetShouldSmoothFonts(ctx, false);
        CGContextSetAllowsFontSmoothing(ctx, false);
        CGContextSetTextPosition(ctx, 0, descent + leading);
        CTLineDraw(line, ctx);
        CGContextFlush(ctx);

        // 6. Un-premultiply alpha. CGBitmapContext data is already top-down
        //    in memory (row 0 = visual top), so no Y-flip needed.
        result.width  = buf_w;
        result.height = buf_h;
        result.baseline = static_cast<i32>(std::lround(buf_h - descent - leading));
        result.pixels.resize(pixels.size());
        for (i32 row = 0; row < buf_h; ++row) {
            const u8* src = &pixels[static_cast<size_t>(row) * row_bytes];
            u8* dst = &result.pixels[static_cast<size_t>(row) * row_bytes];
            for (i32 col = 0; col < buf_w; ++col) {
                u8 pr = src[col * 4 + 0];
                u8 pg = src[col * 4 + 1];
                u8 pb = src[col * 4 + 2];
                u8 pa = src[col * 4 + 3];
                if (pa == 0) {
                    dst[col * 4 + 0] = 0;
                    dst[col * 4 + 1] = 0;
                    dst[col * 4 + 2] = 0;
                    dst[col * 4 + 3] = 0;
                } else {
                    // Un-premultiply: c = (premul * 255) / alpha
                    dst[col * 4 + 0] = static_cast<u8>(std::min(255, (pr * 255 + pa / 2) / pa));
                    dst[col * 4 + 1] = static_cast<u8>(std::min(255, (pg * 255 + pa / 2) / pa));
                    dst[col * 4 + 2] = static_cast<u8>(std::min(255, (pb * 255 + pa / 2) / pa));
                    dst[col * 4 + 3] = pa;
                }
            }
        }

        CGContextRelease(ctx);
        CFRelease(line);
        CFRelease(font);
    }

    return result;
}

// =============================================================================
// measure_text — Core Text text measurement without rendering
// =============================================================================

void CocoaBridge::measure_text(const std::string& utf8_text,
                               float pt_size, u16 weight, bool italic,
                               i32& out_width, i32& out_height,
                               std::vector<i32>* advances, const std::u16string& face_name) {
    out_width  = 0;
    out_height = 0;
    if (advances) advances->clear();
    if (utf8_text.empty()) return;

    @autoreleasepool {
        CTFontSymbolicTraits traits = 0;
        if (weight >= 700) traits |= kCTFontBoldTrait;
        if (italic)        traits |= kCTFontItalicTrait;

        NSString* name = vtr_host_face_for(face_name);
        CTFontRef font = CTFontCreateWithName((__bridge CFStringRef)name, pt_size, nullptr);
        font = vtr_fit_substituted_font(font, face_name, pt_size);
        if (traits != 0) {
            CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(
                font, pt_size, nullptr, traits, traits);
            if (styled) {
                CFRelease(font);
                font = styled;
            }
        }

        // ANSI Win32 strings often contain CP1252 bytes (e.g. ® = 0xAE) that
        // are not valid UTF-8; falling back avoids a 0×0 extent that breaks
        // wizard layout measurement.
        NSString* nsText = [[NSString alloc]
            initWithBytes:utf8_text.data()
                   length:utf8_text.size()
                 encoding:NSUTF8StringEncoding];
        if (!nsText) {
            nsText = [[NSString alloc]
                initWithBytes:utf8_text.data()
                       length:utf8_text.size()
                     encoding:NSISOLatin1StringEncoding];
        }
        if (!nsText) { CFRelease(font); return; }

        NSDictionary* attrs = @{
            (__bridge id)kCTFontAttributeName: (__bridge id)font,
        };
        NSAttributedString* attrStr =
            [[NSAttributedString alloc] initWithString:nsText attributes:attrs];

        CTLineRef line = CTLineCreateWithAttributedString(
            (__bridge CFAttributedStringRef)attrStr);
        CGFloat ascent = 0, descent = 0, leading = 0;
        double advance = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);

        out_width  = static_cast<i32>(std::ceil(advance));
        out_height = static_cast<i32>(std::ceil(ascent + descent + leading));
        if (advances) {
            advances->reserve([nsText length]);
            for (NSUInteger index = 1; index <= [nsText length]; ++index) {
                const CGFloat offset = CTLineGetOffsetForStringIndex(line, index, nullptr);
                advances->push_back(static_cast<i32>(std::ceil(offset)));
            }
        }

        CFRelease(line);
        CFRelease(font);
    }
}

} // namespace vitreo::windowing
