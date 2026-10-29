// SPDX-License-Identifier: LGPL-2.1-only

#import <Cocoa/Cocoa.h>

#include "windowing/cocoa/app_icon_presentation.hpp"

#include <algorithm>

namespace vitreo::windowing {

AppIconPresentationGeometry app_icon_presentation_geometry(
    double width, double height) noexcept {
    if (width <= 0.0 || height <= 0.0) return {};

    const double edge = std::min(width, height);
    const double plate_size = edge * 0.84;
    const double artwork_size = edge * 0.62;
    return {
        .plate_x = (width - plate_size) * 0.5,
        .plate_y = (height - plate_size) * 0.5,
        .plate_size = plate_size,
        .corner_radius = plate_size * 0.22,
        .artwork_x = (width - artwork_size) * 0.5,
        .artwork_y = (height - artwork_size) * 0.5,
        .artwork_size = artwork_size,
    };
}

namespace {

constexpr CGFloat kRenderedIconSize = 1024.0;

NSRect aspect_fit_rect(NSSize source_size, NSRect bounds) {
    if (source_size.width <= 0.0 || source_size.height <= 0.0) return bounds;

    const CGFloat scale =
        std::min(NSWidth(bounds) / source_size.width,
                 NSHeight(bounds) / source_size.height);
    const NSSize fitted =
        NSMakeSize(source_size.width * scale, source_size.height * scale);
    return NSMakeRect(
        NSMidX(bounds) - fitted.width * 0.5,
        NSMidY(bounds) - fitted.height * 0.5,
        fitted.width,
        fitted.height);
}

} // namespace

NSImage* create_macos_presented_app_icon(NSImage* source_icon) {
    if (!source_icon || ![source_icon isValid]) return nil;

    const NSInteger pixel_size = static_cast<NSInteger>(kRenderedIconSize);
    NSBitmapImageRep* representation = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nil
                      pixelsWide:pixel_size
                      pixelsHigh:pixel_size
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSCalibratedRGBColorSpace
                     bytesPerRow:0
                    bitsPerPixel:0];
    if (!representation) return nil;

    const NSSize output_size = NSMakeSize(kRenderedIconSize, kRenderedIconSize);
    [representation setSize:output_size];
    NSGraphicsContext* bitmap_context =
        [NSGraphicsContext graphicsContextWithBitmapImageRep:representation];
    if (!bitmap_context) {
        [representation release];
        return nil;
    }

    const AppIconPresentationGeometry geometry =
        app_icon_presentation_geometry(output_size.width, output_size.height);
    const NSRect plate_rect = NSMakeRect(
        geometry.plate_x, geometry.plate_y,
        geometry.plate_size, geometry.plate_size);
    const NSRect artwork_bounds = NSMakeRect(
        geometry.artwork_x, geometry.artwork_y,
        geometry.artwork_size, geometry.artwork_size);
    NSBezierPath* plate =
        [NSBezierPath bezierPathWithRoundedRect:plate_rect
                                       xRadius:geometry.corner_radius
                                       yRadius:geometry.corner_radius];

    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:bitmap_context];
    [[NSColor clearColor] setFill];
    NSRectFill(NSMakeRect(0.0, 0.0, output_size.width, output_size.height));

    [NSGraphicsContext saveGraphicsState];
    NSShadow* shadow = [[[NSShadow alloc] init] autorelease];
    [shadow setShadowColor:[NSColor colorWithWhite:0.0 alpha:0.32]];
    [shadow setShadowBlurRadius:kRenderedIconSize * 0.035];
    [shadow setShadowOffset:NSMakeSize(0.0, -kRenderedIconSize * 0.012)];
    [shadow set];
    NSGradient* plate_gradient = [[[NSGradient alloc]
        initWithStartingColor:[NSColor colorWithSRGBRed:1.0
                                                 green:1.0
                                                  blue:1.0
                                                 alpha:1.0]
                  endingColor:[NSColor colorWithSRGBRed:0.91
                                                 green:0.92
                                                  blue:0.94
                                                 alpha:1.0]] autorelease];
    [plate_gradient drawInBezierPath:plate angle:-90.0];
    [NSGraphicsContext restoreGraphicsState];

    [[NSColor colorWithWhite:0.0 alpha:0.12] setStroke];
    [plate setLineWidth:kRenderedIconSize * 0.006];
    [plate stroke];

    const NSRect artwork_rect =
        aspect_fit_rect([source_icon size], artwork_bounds);
    [source_icon drawInRect:artwork_rect
                   fromRect:NSZeroRect
                  operation:NSCompositingOperationSourceOver
                   fraction:1.0
             respectFlipped:YES
                      hints:@{NSImageHintInterpolation:
                                  @(NSImageInterpolationHigh)}];

    [bitmap_context flushGraphics];
    [NSGraphicsContext restoreGraphicsState];

    NSImage* result = [[[NSImage alloc] initWithSize:output_size] autorelease];
    [result addRepresentation:representation];
    [representation release];
    return result;
}

} // namespace vitreo::windowing
