// SPDX-License-Identifier: LGPL-2.1-only

#import <Cocoa/Cocoa.h>

#include <cstdlib>

#include <gtest/gtest.h>

#include <windowing/cocoa/app_icon_presentation.hpp>

using namespace vitreo::windowing;

TEST(AppIconPresentationTest, GeometryCentersPlateAndArtwork) {
    const AppIconPresentationGeometry geometry =
        app_icon_presentation_geometry(512.0, 512.0);

    EXPECT_DOUBLE_EQ(geometry.plate_size, 512.0 * 0.84);
    EXPECT_DOUBLE_EQ(geometry.plate_x, geometry.plate_y);
    EXPECT_DOUBLE_EQ(geometry.plate_x * 2.0 + geometry.plate_size, 512.0);
    EXPECT_DOUBLE_EQ(geometry.artwork_x, geometry.artwork_y);
    EXPECT_DOUBLE_EQ(
        geometry.artwork_x * 2.0 + geometry.artwork_size, 512.0);
    EXPECT_LT(geometry.artwork_size, geometry.plate_size);
}

TEST(AppIconPresentationTest, GeometryScalesProportionally) {
    const AppIconPresentationGeometry large =
        app_icon_presentation_geometry(512.0, 512.0);
    const AppIconPresentationGeometry small =
        app_icon_presentation_geometry(128.0, 128.0);

    EXPECT_DOUBLE_EQ(small.plate_x * 4.0, large.plate_x);
    EXPECT_DOUBLE_EQ(small.plate_size * 4.0, large.plate_size);
    EXPECT_DOUBLE_EQ(small.corner_radius * 4.0, large.corner_radius);
    EXPECT_DOUBLE_EQ(small.artwork_x * 4.0, large.artwork_x);
    EXPECT_DOUBLE_EQ(small.artwork_size * 4.0, large.artwork_size);
}

TEST(AppIconPresentationTest, RenderingAddsNeutralPlateAndPreservesArtwork) {
    @autoreleasepool {
        NSImage* source = [[[NSImage alloc]
            initWithSize:NSMakeSize(256.0, 256.0)] autorelease];
        [source lockFocus];
        [[NSColor clearColor] setFill];
        NSRectFill(NSMakeRect(0.0, 0.0, 256.0, 256.0));
        [[NSColor colorWithSRGBRed:0.0
                             green:0.47
                              blue:0.83
                             alpha:1.0] setFill];
        [[NSBezierPath bezierPathWithOvalInRect:
            NSMakeRect(28.0, 28.0, 200.0, 200.0)] fill];
        [source unlockFocus];

        NSImage* result = create_macos_presented_app_icon(source);
        ASSERT_NE(result, nil);
        EXPECT_DOUBLE_EQ([result size].width, 1024.0);
        EXPECT_DOUBLE_EQ([result size].height, 1024.0);

        NSData* tiff = [result TIFFRepresentation];
        ASSERT_NE(tiff, nil);
        NSBitmapImageRep* pixels =
            [NSBitmapImageRep imageRepWithData:tiff];
        ASSERT_NE(pixels, nil);

        NSColor* outside = [[pixels colorAtX:0 y:0]
            colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        ASSERT_NE(outside, nil);
        EXPECT_LT([outside alphaComponent], 0.02);

        NSColor* plate = [[pixels colorAtX:512 y:128]
            colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        ASSERT_NE(plate, nil);
        EXPECT_GT([plate redComponent], 0.80);
        EXPECT_GT([plate greenComponent], 0.80);
        EXPECT_GT([plate blueComponent], 0.80);

        NSColor* artwork = [[pixels colorAtX:512 y:512]
            colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        ASSERT_NE(artwork, nil);
        EXPECT_LT([artwork redComponent], 0.08);
        EXPECT_GT([artwork greenComponent], 0.40);
        EXPECT_GT([artwork blueComponent], 0.75);

        if (const char* preview_path =
                std::getenv("VITREO_MACOS_ICON_PREVIEW");
            preview_path && preview_path[0] != '\0') {
            NSData* png = [pixels
                representationUsingType:NSBitmapImageFileTypePNG
                             properties:@{}];
            ASSERT_NE(png, nil);
            EXPECT_TRUE([png
                writeToFile:[NSString stringWithUTF8String:preview_path]
                  atomically:YES]);
        }
    }
}
