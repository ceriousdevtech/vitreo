// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_codec_imageio.mm
/// @brief GDI+ codec area: the ImageIO-backed pixel paths (JPEG, TIFF and
/// ICO decoding, JPEG encoding, thumbnail scaling).
///
/// Pixels are read from the CGImage's own data provider when its layout is
/// plain 8-bit non-premultiplied RGB/gray (no color conversion, colors of
/// transparent pixels kept, as GDI+ keeps them: probe rgba.tif); otherwise
/// the image is drawn into a premultiplied context in its own color space
/// and un-premultiplied.

#include "winapi/graphics/gdiplus/gdiplus_codec_formats.hpp"
#include "graphics/common/image_codec.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>

namespace vitreo::winapi::gdip::fmt {

namespace {

template <typename T>
struct Cf {
    T ref{nullptr};
    explicit Cf(T r) : ref(r) {}
    ~Cf() { if (ref) CFRelease(ref); }
    Cf(const Cf&) = delete;
    Cf& operator=(const Cf&) = delete;
    explicit operator bool() const { return ref != nullptr; }
};

} // namespace
u32 imageio_count(std::span<const u8> src) {
 return graphics::image_codec::frame_count(src);
}
bool imageio_decode(std::span<const u8> src,u32 index,i32& width,i32& height,std::vector<u32>& argb) {
 return graphics::image_codec::decode(src,index,width,height,argb,
    graphics::image_codec::PixelPolicy::AllowReconstruction)==graphics::image_codec::DecodeResult::Success;
}

bool imageio_encode_jpeg(const std::vector<u32>& argb, i32 width, i32 height, i32 quality, float dpi_x, float dpi_y,
                         std::vector<u8>& out) {
    Cf<CGColorSpaceRef> cs(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    std::vector<u32> px = argb;
    Cf<CGContextRef> ctx(CGBitmapContextCreate(px.data(), static_cast<usize>(width), static_cast<usize>(height), 8,
                                               static_cast<usize>(width) * 4, cs.ref,
                                               static_cast<uint32_t>(kCGImageAlphaNoneSkipFirst) |
                                                   static_cast<uint32_t>(kCGBitmapByteOrder32Little)));
    if (!ctx) return false;
    Cf<CGImageRef> img(CGBitmapContextCreateImage(ctx.ref));
    Cf<CFMutableDataRef> data(CFDataCreateMutable(kCFAllocatorDefault, 0));
    Cf<CGImageDestinationRef> dst(CGImageDestinationCreateWithData(data.ref, CFSTR("public.jpeg"), 1, nullptr));
    if (!img || !dst) return false;
    const double q = quality / 100.0, dx = dpi_x, dy = dpi_y;
    Cf<CFNumberRef> qn(CFNumberCreate(nullptr, kCFNumberDoubleType, &q));
    Cf<CFNumberRef> xn(CFNumberCreate(nullptr, kCFNumberDoubleType, &dx));
    Cf<CFNumberRef> yn(CFNumberCreate(nullptr, kCFNumberDoubleType, &dy));
    const void* keys[] = {kCGImageDestinationLossyCompressionQuality, kCGImagePropertyDPIWidth, kCGImagePropertyDPIHeight};
    const void* vals[] = {qn.ref, xn.ref, yn.ref};
    Cf<CFDictionaryRef> props(CFDictionaryCreate(nullptr, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks));
    CGImageDestinationAddImage(dst.ref, img.ref, props.ref);
    if (!CGImageDestinationFinalize(dst.ref)) return false;
    const u8* p = CFDataGetBytePtr(data.ref);
    out.assign(p, p + CFDataGetLength(data.ref));
    return true;
}

bool imageio_scale(const std::vector<u32>& argb, i32 width, i32 height, i32 w, i32 h, std::vector<u32>& out) {
    Cf<CGColorSpaceRef> cs(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    std::vector<u32> pre = argb;
    for (u32& c : pre) {
        const u32 a = c >> 24;
        auto m = [a](u32 v) { return (v * a + 127) / 255; };
        c = (a << 24) | (m((c >> 16) & 0xFF) << 16) | (m((c >> 8) & 0xFF) << 8) | m(c & 0xFF);
    }
    const uint32_t info = static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little);
    Cf<CGContextRef> src(CGBitmapContextCreate(pre.data(), static_cast<usize>(width), static_cast<usize>(height), 8,
                                               static_cast<usize>(width) * 4, cs.ref, info));
    if (!src) return false;
    Cf<CGImageRef> img(CGBitmapContextCreateImage(src.ref));
    out.assign(static_cast<usize>(w) * static_cast<usize>(h), 0);
    Cf<CGContextRef> dst(CGBitmapContextCreate(out.data(), static_cast<usize>(w), static_cast<usize>(h), 8,
                                               static_cast<usize>(w) * 4, cs.ref, info));
    if (!img || !dst) return false;
    CGContextSetInterpolationQuality(dst.ref, kCGInterpolationHigh);
    CGContextSetBlendMode(dst.ref, kCGBlendModeCopy);
    CGContextDrawImage(dst.ref, CGRectMake(0, 0, w, h), img.ref);
    // out is premultiplied B,G,R,A == the 32bppPARGB thumbnail format.
    return true;
}

}  // namespace vitreo::winapi::gdip::fmt
