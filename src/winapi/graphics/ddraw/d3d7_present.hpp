// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d7_present.hpp
/// @brief Backend render-target readback → DirectDraw surface memory.
///
/// A Direct3D 7 device renders into an IDirectDrawSurface. That surface's
/// pixels are guest-visible: the guest may Lock() them, Blt from them, or
/// present them. So a GPU result is not finished until it lands back in that
/// memory in the surface's own pixel format — a texture the guest cannot read
/// would be invisible to the game.

#include <vitreo/common.hpp>

namespace vitreo::winapi::d3d7 {

/// Copy a BGRA8 readback (the format `BackendTexture2D::readback` produces for
/// a BGRA8Unorm target) into surface memory.
///
/// @param src        readback pixels, @p src_pitch bytes per row
/// @param dst        surface pixels, @p dst_pitch bytes per row
/// @param dst_bpp    surface bits per pixel; 32 and 16 (RGB565) are supported
/// @return false when the destination format has no defined mapping, leaving
///         @p dst untouched rather than writing a wrong interpretation.
[[nodiscard]] bool copy_bgra8_to_surface(const u8* src, u32 src_pitch,
                                         u32 width, u32 height, u8* dst,
                                         u32 dst_pitch, u32 dst_bpp);

} // namespace vitreo::winapi::d3d7
