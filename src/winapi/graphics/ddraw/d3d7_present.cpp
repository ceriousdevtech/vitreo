// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d7_present.cpp
/// @brief Backend render-target readback → DirectDraw surface memory.

#include "winapi/graphics/ddraw/d3d7_present.hpp"

#include <cstring>

namespace vitreo::winapi::d3d7 {

bool copy_bgra8_to_surface(const u8* src, u32 src_pitch, u32 width, u32 height,
                           u8* dst, u32 dst_pitch, u32 dst_bpp) {
    if (src == nullptr || dst == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (src_pitch < width * 4u) return false;

    if (dst_bpp == 32) {
        // Both sides are 8:8:8:8 in B,G,R,A byte order, so the rows copy
        // verbatim. The pitches still differ in general: a DirectDraw surface
        // is allocated with its own alignment.
        if (dst_pitch < width * 4u) return false;
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(dst + static_cast<usize>(y) * dst_pitch,
                        src + static_cast<usize>(y) * src_pitch, width * 4u);
        }
        return true;
    }

    if (dst_bpp == 16) {
        // RGB565: 5 bits red, 6 green, 5 blue. Take the high bits of each
        // channel — the same truncation Direct3D's own 8888→565 conversion
        // performs, rather than rounding, so a fully saturated channel stays
        // saturated.
        if (dst_pitch < width * 2u) return false;
        for (u32 y = 0; y < height; ++y) {
            const u8* src_row = src + static_cast<usize>(y) * src_pitch;
            u8* dst_row = dst + static_cast<usize>(y) * dst_pitch;
            for (u32 x = 0; x < width; ++x) {
                const u8 b = src_row[x * 4 + 0];
                const u8 g = src_row[x * 4 + 1];
                const u8 r = src_row[x * 4 + 2];
                const u16 packed = static_cast<u16>(
                    ((static_cast<u16>(r) >> 3) << 11) |
                    ((static_cast<u16>(g) >> 2) << 5) |
                    (static_cast<u16>(b) >> 3));
                std::memcpy(dst_row + x * 2u, &packed, sizeof(packed));
            }
        }
        return true;
    }

    // Indexed and other depths have no defined mapping from a rendered BGRA8
    // frame; refuse rather than write something the guest would misread.
    return false;
}

} // namespace vitreo::winapi::d3d7
