#pragma once
/// @file gl_pixels.hpp
/// @brief The client-memory side of pixel rectangles (OpenGL 1.1 spec 3.6):
///        unpacking groups under the pixel-store modes (3.6.1, 3.6.4
///        "Unpacking", eq. 3.8-3.10), conversion to floating point (Table
///        2.6), conversion of L to RGB and the final expansion to RGBA, the
///        pixel transfer operations (3.6.4: scale and bias, the RGBA-to-RGBA
///        and index-to-RGBA lookups, index shift and offset), and packing
///        (4.3.2: conversion of L, final conversion per Table 4.7 and placement
///        under the PACK_* modes).
///
/// Texture images (glTexImage*, glTexSubImage*, glCopyTex*, glGetTexImage)
/// use these now; glDrawPixels / glReadPixels / glCopyPixels (Task 11) use
/// the same functions.

#include <vitreo/common.hpp>
#include "gl_context.hpp"

#include <array>
#include <span>
#include <vector>

namespace vitreo::winapi::gl {

using GlRgba = std::array<float, 4>;

/// Table 3.4 types other than BITMAP (and BITMAP when @p allow_bitmap).
[[nodiscard]] bool gl_pixel_type_valid(u32 type, bool allow_bitmap) noexcept;
/// Elements per group of a Table 3.5 format (0 for an unknown format).
[[nodiscard]] u32 gl_pixel_format_elements(u32 format) noexcept;
/// Bytes of one element of @p type (0 for BITMAP or an unknown type).
[[nodiscard]] u32 gl_pixel_type_bytes(u32 type) noexcept;

/// Spec 3.6.4 eq. 3.8-3.10: how a width x height image of @p format /
/// @p type lies in client memory under @p modes, measured from the pointer.
/// Computed in checked arithmetic: when an offset or the span does not fit
/// 64 bits, or the span exceeds the largest user address space
/// (kGlLargestUserAddress), the image cannot be client memory and @p valid is
/// false (gl_client_memory.hpp rule 1). Every reader and packer returns
/// without touching memory for an invalid layout.
struct GlPixelLayout {
    usize first_byte{0};     ///< the first group (SKIP_PIXELS / SKIP_ROWS applied)
    usize row_bytes{0};      ///< k elements (or k ubytes for BITMAP) in bytes
    usize span_bytes{0};     ///< bytes from the pointer to the end of the last group
    u32 skip_bits{0};        ///< BITMAP: SKIP_PIXELS bits before the first element
    bool valid{true};        ///< false: no address range can hold the image
};
[[nodiscard]] GlPixelLayout gl_pixel_layout(const GlPixelStoreModes& modes, u32 format, u32 type, i32 width,
                                            i32 height) noexcept;

/// The groups (x .. , y .. ) of a width x height image under @p modes, as an
/// image of their own: @p region_modes (ROW_LENGTH the full image's, no
/// SKIP_ROWS, SKIP_PIXELS the bit offset for BITMAP) at @p offset bytes from
/// the image's pointer. Packing a w' x h' image under @p region_modes into
/// the image's bytes from @p offset places group (i, j) where group
/// (x + i, y + j) of the full image lies, for x + w' <= width and
/// y + h' <= height. False for an invalid layout or (x, y) outside the image.
[[nodiscard]] bool gl_pixel_region_layout(const GlPixelStoreModes& modes, u32 format, u32 type, i32 width,
                                          i32 height, i32 x, i32 y, GlPixelStoreModes& region_modes,
                                          usize& offset) noexcept;

/// Element (x, y) of a BITMAP image laid out as @p layout (spec 3.6.4: after
/// SKIP_ROWS rows and SKIP_PIXELS bits; MSB first unless LSB_FIRST). @p data
/// holds layout.span_bytes bytes.
[[nodiscard]] bool gl_bitmap_bit(std::span<const u8> data, const GlPixelLayout& layout,
                                 const GlPixelStoreModes& modes, i32 x, i32 y) noexcept;

/// Unpacks a width x height image (rows bottom to top, the first group the
/// lower left) into RGBA groups: conversion to floating point (components) or
/// fixed point (indices), L to RGB, the final expansion, then the pixel
/// transfer operations of @p ctx (RGBA groups are required: index groups go
/// through INDEX_SHIFT/OFFSET and the I_TO_R/G/B/A maps). @p data holds
/// gl_pixel_layout(...).span_bytes bytes. Values are not clamped.
void gl_unpack_rgba(const GlContext& ctx, const GlPixelStoreModes& modes, u32 format, u32 type, i32 width,
                    i32 height, std::span<const u8> data, std::vector<GlRgba>& out);

/// The pixel transfer operations of spec 3.6.4 for one RGBA group: scale and
/// bias, then, when MAP_COLOR, clamp and the R_TO_R .. A_TO_A lookups.
void gl_transfer_rgba(const GlContext& ctx, GlRgba& c) noexcept;

/// Packs width x height RGBA groups (bottom row first) into @p out (which
/// holds gl_pixel_layout(...).span_bytes bytes of the client's buffer, so
/// bytes between groups keep their values) as ReadPixels places them (4.3.2):
/// L = R + G + B for LUMINANCE formats, each value clamped to [0, 1] and
/// converted per Table 4.7, under the PACK_* modes. No pixel transfer.
/// DEPTH_COMPONENT packs each group's first component (the depth value).
/// @p present (empty: every group) marks the groups to store; the client
/// bytes of the others keep their values (glReadPixels outside the window).
void gl_pack_rgba(const GlPixelStoreModes& modes, u32 format, u32 type, i32 width, i32 height,
                  std::span<const GlRgba> groups, std::span<u8> out, std::span<const u8> present = {});

// ---- Index and depth groups (spec 3.6.4, 4.3.2) ----

/// Spec 3.6.4 "Arithmetic on Indices": an index from an integer type keeps
/// no fraction bits (@p integer); it is shifted by |INDEX_SHIFT| bits (left
/// for a positive shift, right otherwise, zero-filled), then INDEX_OFFSET is
/// added.
[[nodiscard]] double gl_index_arithmetic(const GlContext& ctx, double index, bool integer) noexcept;
/// The stencil index path of the pixel transfer operations: index arithmetic,
/// then, when MAP_STENCIL, the PIXEL_MAP_S_TO_S lookup at round(index) &
/// (2^n - 1) ("Stencil Index Lookup"). Not masked (the final conversion does
/// that).
[[nodiscard]] double gl_transfer_stencil(const GlContext& ctx, double index, bool integer) noexcept;
/// The depth path: DEPTH_SCALE and DEPTH_BIAS ("Arithmetic on Components").
[[nodiscard]] float gl_transfer_depth(const GlContext& ctx, float depth) noexcept;

/// Unpacks a STENCIL_INDEX image (any type, BITMAP included) through the
/// stencil transfer path (gl_transfer_stencil). Bottom row first. Values are
/// not masked.
void gl_unpack_stencil(const GlContext& ctx, const GlPixelStoreModes& modes, u32 type, i32 width, i32 height,
                       std::span<const u8> data, std::vector<double>& out);
/// Unpacks a COLOR_INDEX image as indices (spec 3.6.4 "Arithmetic on
/// Indices", then, when MAP_COLOR, the I_TO_I lookup): what a
/// GL_EXT_paletted_texture COLOR_INDEXn_EXT image keeps. Bottom row first.
void gl_unpack_color_indices(const GlContext& ctx, const GlPixelStoreModes& modes, u32 type, i32 width, i32 height,
                             std::span<const u8> data, std::vector<double>& out);
/// Unpacks a DEPTH_COMPONENT image: Table 2.6 conversion to floating point,
/// DEPTH_SCALE / DEPTH_BIAS, then the final conversion's clamp to [0, 1].
void gl_unpack_depth(const GlContext& ctx, const GlPixelStoreModes& modes, u32 type, i32 width, i32 height,
                     std::span<const u8> data, std::vector<float>& out);

/// Packs indices (glReadPixels of STENCIL_INDEX) as 4.3.2's final conversion
/// places them: an integer type stores the index masked by Table 4.6
/// (UNSIGNED_BYTE 2^8 - 1, BYTE 2^7 - 1, UNSIGNED_SHORT 2^16 - 1, SHORT 2^15 -
/// 1, UNSIGNED_INT 2^32 - 1, INT 2^31 - 1, BITMAP 1), FLOAT stores the index
/// as a float. BITMAP sets or clears one bit per index under PACK_LSB_FIRST
/// and PACK_SKIP_PIXELS, leaving the other bits of the byte. @p present as
/// for gl_pack_rgba.
void gl_pack_indices(const GlPixelStoreModes& modes, u32 type, i32 width, i32 height, std::span<const double> indices,
                     std::span<u8> out, std::span<const u8> present = {});

}  // namespace vitreo::winapi::gl
