// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <span>
#include <vector>
#include <vitreo/common.hpp>
namespace vitreo::graphics::image_codec {
enum class DecodeResult { Success, InvalidImage, UnsupportedLayout };
// NativeBytes rejects premultiplication reconstruction and preserves RGB at
// A=0.
enum class PixelPolicy { NativeBytes, AllowReconstruction };
// Metadata only: never creates/decodes an image raster. Windows frontends
// select their own native format and defaults from these source facts.
enum class ColorModel { Unknown, Rgb, Gray, Cmyk };
struct Info {
  u32 width{}, height{}, depth{};
  ColorModel color{ColorModel::Unknown};
  double dpi_x{}, dpi_y{};
  bool has_dpi{}; // an explicitly stored zero density is distinct from absent metadata
};
DecodeResult info(std::span<const u8>, u32 index, Info&);
u32 frame_count(std::span<const u8>);
DecodeResult decode(std::span<const u8>, u32 index, i32 &width, i32 &height,
                    std::vector<u32> &argb, PixelPolicy);
} // namespace vitreo::graphics::image_codec
