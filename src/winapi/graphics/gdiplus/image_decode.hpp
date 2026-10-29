// SPDX-License-Identifier: LGPL-2.1-only

/// @file image_decode.hpp
/// @brief Raster image decoding for GDI+.
///
/// GDI+ decodes BMP, GIF, JPEG, PNG, TIFF and ICO. Vitreo has no decoder of its
/// own, so this wraps the host's ImageIO, which reads the same family. The
/// interface is deliberately plain C++ so callers do not need Objective-C++.
///
/// Output is always top-down, 8 bits per channel, non-premultiplied BGRA in the
/// byte order GDI+ calls PixelFormat32bppARGB: on a little-endian machine the
/// bytes run B, G, R, A, which is also what a Windows 32bpp DIB holds. Callers
/// that need RGBA (Vitreo's SoftBitmap) swap the red and blue channels.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "vitreo/types.hpp"

namespace vitreo::winapi::gdiplus_decode {

struct DecodedImage {
    u32 width  = 0;
    u32 height = 0;
    /// width * height * 4 bytes, top-down, B,G,R,A per pixel.
    std::vector<u8> bgra;
};

/// Decode an encoded image. Returns nullopt when the bytes are not a format the
/// host can read, or when the image is implausibly large.
[[nodiscard]] std::optional<DecodedImage> decode(std::span<const u8> encoded);

/// True when image decoding is available at all on this build. Lets callers
/// report GdipNotImplemented rather than a decode failure when the platform
/// provides no codec.
[[nodiscard]] bool available() noexcept;

} // namespace vitreo::winapi::gdiplus_decode
