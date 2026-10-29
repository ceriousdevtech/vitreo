// SPDX-License-Identifier: LGPL-2.1-only
// Guest-independent ImageIO leaf. Frontends own their Windows codec policy.
#include "graphics/common/image_codec.hpp"
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <algorithm>
#include <limits>
namespace vitreo::graphics::image_codec {
namespace {
#include "jpeg_density_contract.inc"
template <typename T> struct Cf {
  T ref{nullptr};
  explicit Cf(T r) : ref(r) {}
  ~Cf() {
    if (ref)
      CFRelease(ref);
  }
  Cf(const Cf &) = delete;
  Cf &operator=(const Cf &) = delete;
  explicit operator bool() const { return ref != nullptr; }
};

constexpr usize kMaxDimension = 32768;

CGImageSourceRef source_of(std::span<const u8> src) {
  Cf<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, src.data(),
                                  static_cast<CFIndex>(src.size())));
  if (!data)
    return nullptr;
  return CGImageSourceCreateWithData(data.ref, nullptr);
}

bool raw_pixels(CGImageRef img, usize w, usize h, std::vector<u32> &argb) {
  if (CGImageGetBitsPerComponent(img) != 8)
    return false;
  CGColorSpaceRef cs = CGImageGetColorSpace(img);
  if (!cs)
    return false;
  const CGColorSpaceModel model = CGColorSpaceGetModel(cs);
  const CGImageAlphaInfo alpha = CGImageGetAlphaInfo(img);
  const CGBitmapInfo order = CGImageGetBitmapInfo(img) & kCGBitmapByteOrderMask;
  if (order != kCGBitmapByteOrderDefault && order != kCGBitmapByteOrder32Big)
    return false;
  if (alpha == kCGImageAlphaPremultipliedFirst ||
      alpha == kCGImageAlphaPremultipliedLast || alpha == kCGImageAlphaOnly)
    return false;
  const usize bpp = CGImageGetBitsPerPixel(img) / 8;
  const usize comps = model == kCGColorSpaceModelRGB          ? 3
                      : model == kCGColorSpaceModelMonochrome ? 1
                                                              : 0;
  if (comps == 0)
    return false;
  const bool has_alpha =
      alpha == kCGImageAlphaLast || alpha == kCGImageAlphaFirst;
  const bool skip =
      alpha == kCGImageAlphaNoneSkipFirst || alpha == kCGImageAlphaNoneSkipLast;
  const usize expect = comps + (has_alpha || skip ? 1 : 0);
  if (bpp != expect)
    return false;
  const bool first =
      alpha == kCGImageAlphaFirst || alpha == kCGImageAlphaNoneSkipFirst;
  Cf<CFDataRef> data(CGDataProviderCopyData(CGImageGetDataProvider(img)));
  if (!data)
    return false;
  const u8 *p = CFDataGetBytePtr(data.ref);
  const usize row = CGImageGetBytesPerRow(img);
  if (static_cast<usize>(CFDataGetLength(data.ref)) < row * (h - 1) + w * bpp)
    return false;
  argb.resize(w * h);
  for (usize y = 0; y < h; ++y) {
    const u8 *r = p + y * row;
    for (usize x = 0; x < w; ++x) {
      const u8 *q = r + x * bpp;
      const u8 *c = first ? q + 1 : q;
      u32 a = 0xFF;
      if (has_alpha)
        a = first ? q[0] : q[comps];
      const u32 R = c[0], G = comps == 3 ? c[1] : c[0],
                B = comps == 3 ? c[2] : c[0];
      argb[y * w + x] = (a << 24) | (R << 16) | (G << 8) | B;
    }
  }
  return true;
}

bool drawn_pixels(CGImageRef img, usize w, usize h, std::vector<u32> &argb) {
  CGColorSpaceRef own = CGImageGetColorSpace(img);
  Cf<CGColorSpaceRef> srgb(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
  CGColorSpaceRef cs = own && CGColorSpaceGetModel(own) == kCGColorSpaceModelRGB
                           ? own
                           : srgb.ref;
  argb.assign(w * h, 0);
  Cf<CGContextRef> ctx(CGBitmapContextCreate(
      argb.data(), w, h, 8, w * 4, cs,
      static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) |
          static_cast<uint32_t>(kCGBitmapByteOrder32Little)));
  if (!ctx)
    return false;
  CGContextSetBlendMode(ctx.ref, kCGBlendModeCopy);
  CGContextDrawImage(
      ctx.ref,
      CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)), img);
  for (u32 &c : argb) {
    const u32 a = c >> 24;
    if (a == 0 || a == 255) {
      if (a == 0)
        c = 0;
      continue;
    }
    auto un = [a](u32 v) { return std::min<u32>(255, (v * 255 + a / 2) / a); };
    c = (a << 24) | (un((c >> 16) & 0xFF) << 16) | (un((c >> 8) & 0xFF) << 8) |
        un(c & 0xFF);
  }
  return true;
}

} // namespace
DecodeResult info(std::span<const u8> src, u32 index, Info& out) {
  Cf<CGImageSourceRef> source(source_of(src));
  if (!source || index >= CGImageSourceGetCount(source.ref))
    return DecodeResult::InvalidImage;
  Cf<CFDictionaryRef> properties(CGImageSourceCopyPropertiesAtIndex(source.ref,index,nullptr));
  if (!properties) return DecodeResult::InvalidImage;
  const auto number_from=[&](CFDictionaryRef dictionary,CFStringRef key) {
    double value=0;
    auto raw=CFDictionaryGetValue(dictionary,key);
    if(raw && CFGetTypeID(raw)==CFNumberGetTypeID())
      CFNumberGetValue(static_cast<CFNumberRef>(raw),kCFNumberDoubleType,&value);
    return value;
  };
  const auto number=[&](CFStringRef key){return number_from(properties.ref,key);};
  const double width=number(kCGImagePropertyPixelWidth),height=number(kCGImagePropertyPixelHeight);
  if(width<1||height<1||width>std::numeric_limits<u32>::max()||height>std::numeric_limits<u32>::max())
    return DecodeResult::InvalidImage;
  out={};out.width=static_cast<u32>(width);out.height=static_cast<u32>(height);
  out.depth=static_cast<u32>(number(kCGImagePropertyDepth));
  out.dpi_x=number(kCGImagePropertyDPIWidth);out.dpi_y=number(kCGImagePropertyDPIHeight);
  out.has_dpi=CFDictionaryContainsKey(properties.ref,kCGImagePropertyDPIWidth)&&CFDictionaryContainsKey(properties.ref,kCGImagePropertyDPIHeight);
  // ImageIO omits physical DPI properties when a JFIF coordinate is zero.
  // Preserve the original source density rather than substituting a default.
  // JFIF 1.02 p5: unit1=inches, unit2=centimeters (2.54 cm/inch).
  if(!out.has_dpi){
    auto raw=CFDictionaryGetValue(properties.ref,kCGImagePropertyJFIFDictionary);
    if(raw&&CFGetTypeID(raw)==CFDictionaryGetTypeID()){
      auto jfif=static_cast<CFDictionaryRef>(raw);const auto unit=number_from(jfif,kCGImagePropertyJFIFDensityUnit);
      if(unit==JfifDotsPerInch||unit==JfifDotsPerCentimeter){const double scale=unit==JfifDotsPerCentimeter?2.54:1;out.dpi_x=number_from(jfif,kCGImagePropertyJFIFXDensity)*scale;out.dpi_y=number_from(jfif,kCGImagePropertyJFIFYDensity)*scale;out.has_dpi=true;}
    }
  }
  auto color=CFDictionaryGetValue(properties.ref,kCGImagePropertyColorModel);
  if(color && CFEqual(color,kCGImagePropertyColorModelRGB))out.color=ColorModel::Rgb;
  else if(color && CFEqual(color,kCGImagePropertyColorModelGray))out.color=ColorModel::Gray;
  else if(color && CFEqual(color,kCGImagePropertyColorModelCMYK))out.color=ColorModel::Cmyk;
  return DecodeResult::Success;
}
u32 frame_count(std::span<const u8> src) {
  Cf<CGImageSourceRef> source(source_of(src));
  return source ? static_cast<u32>(CGImageSourceGetCount(source.ref)) : 0;
}
DecodeResult decode(std::span<const u8> src, u32 index, i32 &width, i32 &height,
                    std::vector<u32> &argb, PixelPolicy policy) {
  Cf<CGImageSourceRef> source(source_of(src));
  if (!source || index >= CGImageSourceGetCount(source.ref))
    return DecodeResult::InvalidImage;
  Cf<CGImageRef> image(
      CGImageSourceCreateImageAtIndex(source.ref, index, nullptr));
  if (!image)
    return DecodeResult::InvalidImage;
  const usize w = CGImageGetWidth(image.ref), h = CGImageGetHeight(image.ref);
  if (!w || !h || w > kMaxDimension || h > kMaxDimension)
    return DecodeResult::InvalidImage;
  if (!raw_pixels(image.ref, w, h, argb)) {
    if (policy == PixelPolicy::NativeBytes)
      return DecodeResult::UnsupportedLayout;
    if (!drawn_pixels(image.ref, w, h, argb))
      return DecodeResult::InvalidImage;
  }
  width = static_cast<i32>(w);
  height = static_cast<i32>(h);
  return DecodeResult::Success;
}
} // namespace vitreo::graphics::image_codec
