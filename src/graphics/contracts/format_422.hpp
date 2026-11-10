// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file format_422.hpp
/// @brief The guest encoding of DXGI R8G8_B8G8_UNORM / G8R8_G8B8_UNORM and
/// the decoded RGBA8 storage that holds them (PixelFormat::RGBG422Unorm /
/// GRGB422Unorm).
///
/// Learn DXGI_FORMAT: R8G8_B8G8_UNORM "is analogous to the UYVY format. Each
/// 32-bit block describes a pair of pixels: (R8, G8, B8) and (R8, G8, B8)
/// where the R8/B8 values are repeated, and the G8 values are unique to each
/// pixel"; G8R8_G8B8_UNORM is "analogous to the YUY2 format". Components are
/// named from the lowest byte, so the R8G8_B8G8 block is the bytes
/// (R, G0, B, G1) and the G8R8_G8B8 block (G0, R, G1, B). Metal's BGRG422 /
/// GBGR422 decode exactly these with R and B exchanged (measured,
/// tools/oracle_probe/metal/metal_422_facts.m).
///
/// A decoded pixel is RGBA8 (R, G, B, G'), where G' is the G of the other
/// pixel of its block: either pixel then re-encodes the whole block, which
/// keeps the block of an odd-width level (whose second pixel has no storage)
/// byte for byte. Shader views read alpha as 1.

#include <vitreo/common.hpp>

#include "graphics/contracts/format_converter.hpp"

#include <cstring>

namespace vitreo::graphics {

[[nodiscard]] inline constexpr bool is_422_storage(PixelFormat format) noexcept {
    return format == PixelFormat::RGBG422Unorm ||
           format == PixelFormat::GRGB422Unorm;
}

/// Byte positions of R, G0, B, G1 within a guest block of @p format.
struct Block422Layout {
    u32 r, g0, b, g1;
};

[[nodiscard]] inline constexpr Block422Layout block_422_layout(
    PixelFormat format) noexcept {
    return format == PixelFormat::GRGB422Unorm ? Block422Layout{1, 0, 3, 2}
                                               : Block422Layout{0, 1, 2, 3};
}

/// Decodes @p rows rows of guest blocks into decoded texels. @p x is the
/// first pixel (even), @p width the pixel count written (the level's own
/// pixels: an odd count writes the first pixel of the last block only).
inline void decode_422_rows(PixelFormat format, const u8* src, usize src_pitch,
                            u8* dst, usize dst_pitch, u32 width, u32 rows) {
    const Block422Layout l = block_422_layout(format);
    for (u32 y = 0; y < rows; ++y) {
        const u8* in = src + y * src_pitch;
        u8* out = dst + y * dst_pitch;
        for (u32 px = 0; px < width; ++px) {
            const u8* block = in + (px / 2u) * 4u;
            const bool second = (px & 1u) != 0;
            out[px * 4u + 0] = block[l.r];
            out[px * 4u + 1] = block[second ? l.g1 : l.g0];
            out[px * 4u + 2] = block[l.b];
            out[px * 4u + 3] = block[second ? l.g0 : l.g1];
        }
    }
}

/// Encodes decoded texels back into guest blocks: (width + 1) / 2 blocks per
/// row, each from its first pixel.
inline void encode_422_rows(PixelFormat format, const u8* src, usize src_pitch,
                            u8* dst, usize dst_pitch, u32 width, u32 rows) {
    const Block422Layout l = block_422_layout(format);
    for (u32 y = 0; y < rows; ++y) {
        const u8* in = src + y * src_pitch;
        u8* out = dst + y * dst_pitch;
        for (u32 block = 0; block < (width + 1u) / 2u; ++block) {
            const u8* px = in + block * 8u;
            u8* o = out + block * 4u;
            o[l.r] = px[0];
            o[l.g0] = px[1];
            o[l.b] = px[2];
            o[l.g1] = px[3];
        }
    }
}

}  // namespace vitreo::graphics
