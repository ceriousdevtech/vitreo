#pragma once
/// @file d3d11_standard_swizzle.hpp
/// @brief The CPU-visible address mapping of D3D11_TEXTURE_LAYOUT_64K_STANDARD_SWIZZLE.
///
/// Learn (D3D11_TEXTURE_LAYOUT): "The standard swizzle formats applies within
/// each page-sized chunk, and pages are laid out in linear order with respect
/// to one another. A 16-bit interleave pattern defines the conversion from
/// pre-swizzled intra-page location to the post-swizzled location":
///
///     swizzledOffset = pageOffset + pdep(xBytes, xMask) + pdep(y, yMask)
///                      (+ pdep(z, zMask) for a volume)
///
/// where xBytes is the byte offset of the element within the page's row. Learn
/// prints the 2D 32bpp masks (xBytesMask 1010 1010 1000 1111, yMask 0101 0101
/// 0111 0000) and draws the rest; every mask below was MEASURED on Windows
/// (WARP): tools/oracle_probe/d3d11_texmap_probe [swz] writes one element at
/// every power-of-two offset of a mapping and locates it through a GPU copy,
/// for elements of 1, 2, 4, 8 and 16 bytes (BC1/BC4 blocks behave as 8-byte
/// elements, BC2/3/5/6H/7 as 16-byte ones, packed 4:2:2 pixel pairs as 4-byte
/// ones), in a Texture2D and a Texture3D. The same probe's CRCs of whole
/// mappings (partial tiles, mips, array slices) check the result byte for byte.
///
/// Measured layout of a subresource (the [pitch] lines): a 64 KB page ("tile")
/// holds tile_w x tile_h (x tile_d) elements; the subresource is
/// tiles_x x tiles_y (x tiles_z) whole pages, row of pages first; every mip
/// and array slice is its own subresource with its own pages (no packed mip
/// tail). Map reports RowPitch = tiles_x * 64 KB (one row of pages) and
/// DepthPitch = tiles_x * tiles_y * 64 KB (one slab of pages); a volume has
/// tiles_z slabs.
///
/// Pure functions, so the unit tests hold them to the oracle's offsets.

#include <vitreo/types.hpp>

namespace vitreo::winapi::d3d11_swizzle {

inline constexpr u32 kPageBytes = 65536;

/// Deposits the low bits of @p value at the set bits of @p mask, lowest
/// first (the BMI2 PDEP operation the Learn formula names).
[[nodiscard]] constexpr u32 pdep(u32 value, u32 mask) noexcept {
    u32 result = 0;
    for (u32 bit = 1; mask != 0; bit <<= 1) {
        const u32 lowest = mask & (0u - mask);
        if ((value & bit) != 0) result |= lowest;
        mask &= mask - 1;
    }
    return result;
}

[[nodiscard]] constexpr u32 popcount16(u32 mask) noexcept {
    u32 count = 0;
    for (; mask != 0; mask &= mask - 1) ++count;
    return count;
}

/// The interleave masks for an element of @p element_bytes (1, 2, 4, 8 or 16)
/// in a 2D or 3D resource; false for any other element size (no standard
/// swizzle exists for it -- WARP refuses 96-bit formats with CPU access).
struct Masks {
    u32 x_bytes{0};
    u32 y{0};
    u32 z{0};
};
[[nodiscard]] constexpr bool masks_for(u32 element_bytes, bool volume,
                                       Masks& out) noexcept {
    // Measured offsets -> element coordinates (d3d11_texmap_probe [swz]):
    //   2D  1 byte : x0-3 y0-4 x4 y5 x5 y6 x6 y7 x7            (256x256)
    //   2D  2/4    : xb0-3 y0-2 xb4 y3 xb5 y4 xb6 y5 xb7 y6 xb8 (256x128 /
    //                128x128 elements)
    //   2D  8/16   : xb0-3 y0 y1 xb4 xb5 y2 xb6 y3 xb7 y4 xb8 y5 xb9
    //                (128x64 / 64x64)
    //   3D  1/2    : xb0-3 y0 y1 z0 z1 z2 y2 z3 y3 xb4 z4 y4 xb5
    //                (64x32x32 / 32x32x32)
    //   3D  4      : xb0-3 y0 y1 z0 z1 y2 xb4 z2 y3 xb5 z3 y4 xb6 (32x32x16)
    //   3D  8/16   : xb0-3 y0 y1 z0 z1 xb4 xb5 z2 y2 xb6 z3 y3 xb7
    //                (32x16x16 / 16x16x16)
    if (!volume) {
        switch (element_bytes) {
        case 1: out = {0xAA0Fu, 0x55F0u, 0u}; return true;
        case 2:
        case 4: out = {0xAA8Fu, 0x5570u, 0u}; return true;
        case 8:
        case 16: out = {0xAACFu, 0x5530u, 0u}; return true;
        default: return false;
        }
    }
    switch (element_bytes) {
    case 1:
    case 2: out = {0x900Fu, 0x4A30u, 0x25C0u}; return true;
    case 4: out = {0x920Fu, 0x4930u, 0x24C0u}; return true;
    case 8:
    case 16: out = {0x930Fu, 0x4830u, 0x24C0u}; return true;
    default: return false;
    }
}

/// One subresource's standard-swizzle layout. Extents are in elements (texels,
/// 4x4 blocks, or packed pixel pairs).
struct Layout {
    u32 element_bytes{0};
    Masks masks{};
    u32 tile_w{1}, tile_h{1}, tile_d{1};
    u32 tiles_x{1}, tiles_y{1}, tiles_z{1};

    [[nodiscard]] constexpr u64 row_pitch() const noexcept {
        return static_cast<u64>(tiles_x) * kPageBytes;
    }
    [[nodiscard]] constexpr u64 depth_pitch() const noexcept {
        return row_pitch() * tiles_y;
    }
    [[nodiscard]] constexpr u64 size() const noexcept {
        return depth_pitch() * tiles_z;
    }
    /// Byte offset of the first byte of element (x, y, z); the element's
    /// bytes follow it contiguously (every mask keeps xBytes bits 0-3 at
    /// offset bits 0-3).
    [[nodiscard]] constexpr u64 offset(u32 x, u32 y, u32 z) const noexcept {
        const u64 page =
            (static_cast<u64>(z / tile_d) * tiles_y + y / tile_h) * tiles_x +
            x / tile_w;
        return page * kPageBytes +
               pdep((x % tile_w) * element_bytes, masks.x_bytes) +
               pdep(y % tile_h, masks.y) + pdep(z % tile_d, masks.z);
    }
};

/// The layout of a subresource of @p elements_w x @p elements_h x @p depth
/// elements of @p element_bytes; false when no standard swizzle exists for
/// the element size.
[[nodiscard]] constexpr bool layout_for(u32 element_bytes, bool volume,
                                        u32 elements_w, u32 elements_h,
                                        u32 depth, Layout& out) noexcept {
    Masks masks{};
    if (!masks_for(element_bytes, volume, masks)) return false;
    out = Layout{};
    out.element_bytes = element_bytes;
    out.masks = masks;
    out.tile_w = (1u << popcount16(masks.x_bytes)) / element_bytes;
    out.tile_h = 1u << popcount16(masks.y);
    out.tile_d = volume ? 1u << popcount16(masks.z) : 1u;
    out.tiles_x = (elements_w + out.tile_w - 1) / out.tile_w;
    out.tiles_y = (elements_h + out.tile_h - 1) / out.tile_h;
    out.tiles_z = volume ? (depth + out.tile_d - 1) / out.tile_d : 1u;
    if (out.tiles_x == 0) out.tiles_x = 1;
    if (out.tiles_y == 0) out.tiles_y = 1;
    if (out.tiles_z == 0) out.tiles_z = 1;
    return true;
}

}  // namespace vitreo::winapi::d3d11_swizzle
