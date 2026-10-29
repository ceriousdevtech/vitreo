// SPDX-License-Identifier: LGPL-2.1-only

/// @file image_decode.mm
/// @brief ImageIO-backed raster decoding for GDI+.

#include "winapi/graphics/gdiplus/image_decode.hpp"

#include <vitreo/common.hpp>

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

namespace vitreo::winapi::gdiplus_decode {

namespace {

/// A decoded image this large is far past anything a UI resource contains and
/// would be a 1 GiB allocation; refuse rather than decode it.
constexpr u32 kMaxDimension = 16384u;

/// RAII for CoreFoundation objects, which ImageIO hands back +1.
template <typename T>
class CfRef {
public:
    explicit CfRef(T ref) : m_ref(ref) {}
    ~CfRef() { if (m_ref) CFRelease(m_ref); }
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
    [[nodiscard]] T get() const { return m_ref; }
    explicit operator bool() const { return m_ref != nullptr; }
private:
    T m_ref;
};

} // namespace

bool available() noexcept { return true; }

std::optional<DecodedImage> decode(std::span<const u8> encoded) {
    if (encoded.empty()) return std::nullopt;

    CfRef<CFDataRef> data(CFDataCreate(
        kCFAllocatorDefault, encoded.data(),
        static_cast<CFIndex>(encoded.size())));
    if (!data) return std::nullopt;

    CfRef<CGImageSourceRef> source(
        CGImageSourceCreateWithData(data.get(), nullptr));
    if (!source) return std::nullopt;
    if (CGImageSourceGetCount(source.get()) == 0) return std::nullopt;

    // Frame 0. GDI+ also exposes multi-frame images (animated GIF, multi-page
    // TIFF) through the frame-dimension APIs; those are separate entry points
    // Vitreo does not implement, so decoding the first frame here is the whole
    // of what the single-image path is defined to return.
    CfRef<CGImageRef> image(
        CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr));
    if (!image) return std::nullopt;

    const usize width  = CGImageGetWidth(image.get());
    const usize height = CGImageGetHeight(image.get());
    if (width == 0 || height == 0) return std::nullopt;
    if (width > kMaxDimension || height > kMaxDimension) {
        VITREO_TRACE("[gdiplus] decode: refusing %zux%zu image\n", width,
                     height);
        return std::nullopt;
    }

    DecodedImage out;
    out.width  = static_cast<u32>(width);
    out.height = static_cast<u32>(height);
    out.bgra.assign(width * height * 4u, 0);

    CfRef<CGColorSpaceRef> space(CGColorSpaceCreateDeviceRGB());
    if (!space) return std::nullopt;

    // CGBitmapContext supports only premultiplied or no alpha — the
    // non-premultiplied variants are rejected outright, which silently fails
    // the whole decode. Render premultiplied here and undo it below.
    // PremultipliedFirst + byte-order-32-little puts the bytes in memory as
    // B,G,R,A, which is the 32bppARGB layout GDI+ reports.
    CfRef<CGContextRef> ctx(CGBitmapContextCreate(
        out.bgra.data(), width, height, 8, width * 4u, space.get(),
        static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) |
            static_cast<uint32_t>(kCGBitmapByteOrder32Little)));
    if (!ctx) return std::nullopt;

    // No CTM flip. A bitmap context's buffer already starts at the image's top
    // row, and CGContextDrawImage into the full-size rect lands right way up;
    // adding the usual translate+scale(-1,1) inverts it instead of correcting
    // it, which the 2x2 fixture catches because its corners all differ.
    CGContextSetBlendMode(ctx.get(), kCGBlendModeCopy);
    CGContextDrawImage(
        ctx.get(),
        CGRectMake(0, 0, static_cast<CGFloat>(width),
                   static_cast<CGFloat>(height)),
        image.get());

    // Back to straight alpha. GDI+ hands callers un-premultiplied channels
    // unless they explicitly asked for PixelFormat32bppPARGB; leaving the
    // premultiplied values in place darkens every partially transparent pixel
    // as soon as it is composited a second time.
    for (usize i = 0; i + 3 < out.bgra.size(); i += 4) {
        const u32 a = out.bgra[i + 3];
        if (a == 0) {
            // Fully transparent: the colour carries no information, and
            // dividing by zero alpha has no defined inverse.
            out.bgra[i + 0] = 0;
            out.bgra[i + 1] = 0;
            out.bgra[i + 2] = 0;
            continue;
        }
        if (a == 255u) continue;
        for (usize c = 0; c < 3; ++c) {
            const u32 v = (static_cast<u32>(out.bgra[i + c]) * 255u + a / 2u) / a;
            out.bgra[i + c] = static_cast<u8>(v > 255u ? 255u : v);
        }
    }

    VITREO_TRACE("[gdiplus] decoded %ux%u image (%zu bytes in)\n", out.width,
                 out.height, encoded.size());
    return out;
}

} // namespace vitreo::winapi::gdiplus_decode
