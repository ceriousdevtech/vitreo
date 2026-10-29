#pragma once
/// @file gl_pixel_formats.hpp
/// @brief The pixel formats Vitreo's OpenGL ICD offers, ChoosePixelFormat's
///        selection rule, and the mapping to the context's framebuffer.
///
/// Vitreo acts as the system's OpenGL driver (an ICD), so the list holds only
/// formats it can render: window formats with 32-bit BGRA colour on the
/// shared backend, single or double buffered, no accumulation buffer. There
/// are no PFD_GENERIC_FORMAT, PFD_DRAW_TO_BITMAP or colour-index formats.
///
/// ChoosePixelFormat follows the six-level key measured on Windows 11
/// (tools/oracle_probe/wgl_contract_probe64.expected, header comment),
/// applied to this list instead of Microsoft's 36 generic formats.

#include <vitreo/common.hpp>
#include "graphics/contracts/format_converter.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"

#include <array>
#include <vector>

namespace vitreo::winapi::gl {

// PIXELFORMATDESCRIPTOR dwFlags (wingdi.h).
inline constexpr u32 PFD_DOUBLEBUFFER = 0x00000001;
inline constexpr u32 PFD_STEREO = 0x00000002;
inline constexpr u32 PFD_DRAW_TO_WINDOW = 0x00000004;
inline constexpr u32 PFD_DRAW_TO_BITMAP = 0x00000008;
inline constexpr u32 PFD_SUPPORT_GDI = 0x00000010;
inline constexpr u32 PFD_SUPPORT_OPENGL = 0x00000020;
inline constexpr u32 PFD_GENERIC_FORMAT = 0x00000040;
inline constexpr u32 PFD_NEED_PALETTE = 0x00000080;
inline constexpr u32 PFD_SWAP_EXCHANGE = 0x00000200;
inline constexpr u32 PFD_SWAP_COPY = 0x00000400;
inline constexpr u32 PFD_SUPPORT_COMPOSITION = 0x00008000;
// Request-only flags.
inline constexpr u32 PFD_DEPTH_DONTCARE = 0x20000000;
inline constexpr u32 PFD_DOUBLEBUFFER_DONTCARE = 0x40000000;
inline constexpr u32 PFD_STEREO_DONTCARE = 0x80000000;

inline constexpr u32 PFD_TYPE_RGBA = 0;
inline constexpr u32 PFD_TYPE_COLORINDEX = 1;
inline constexpr u32 kPixelFormatDescriptorSize = 40;

/// One entry of the format list.
struct GlPixelFormat {
    u32 flags;
    u8 color_bits;
    u8 red_bits;
    u8 green_bits;
    u8 blue_bits;
    u8 alpha_bits;
    u8 depth_bits;
    u8 stencil_bits;
    bool double_buffer;
};

/// The fields of a PIXELFORMATDESCRIPTOR that ChoosePixelFormat reads
/// (stencil is read by Vitreo's tie-break only; Windows' 36 formats all have
/// 8 stencil bits, so its effect there is unobservable).
struct PixelFormatRequest {
    u32 flags{0};
    u8 pixel_type{PFD_TYPE_RGBA};
    u8 color_bits{0};
    u8 alpha_bits{0};
    u8 accum_bits{0};
    u8 depth_bits{0};
    u8 stencil_bits{0};
};

/// The pixel formats, 1-based by position: format N is element N - 1.
[[nodiscard]] const std::vector<GlPixelFormat>& gl_pixel_formats();

/// The format at 1-based @p index, or nullptr when out of range.
[[nodiscard]] const GlPixelFormat* gl_pixel_format(i32 index);

/// ChoosePixelFormat: the 1-based index of the best format for @p request.
/// Never fails: every request gets a format.
[[nodiscard]] i32 choose_gl_pixel_format(const PixelFormatRequest& request);

/// The 40-byte PIXELFORMATDESCRIPTOR for @p format (nSize 40, nVersion 1).
[[nodiscard]] std::array<u8, kPixelFormatDescriptorSize>
gl_pixel_format_descriptor(const GlPixelFormat& format);

/// The framebuffer a context created on @p format has (what glGet of
/// GL_RED_BITS ... GL_DOUBLEBUFFER reports). INDEX_BITS is 8 (measured on a
/// 32-bit RGBA window format: gl11_state_probe64.expected get-GL_INDEX_BITS);
/// accumulation bits are the descriptor's (cAccumBits 0), no aux, no stereo.
[[nodiscard]] GlFramebufferConfig gl_framebuffer_config(const GlPixelFormat& format);

/// The backend textures a drawable of @p format needs.
struct GlBackendTargets {
    graphics::PixelFormat color;          ///< back and front buffer
    graphics::PixelFormat depth_stencil;  ///< Unknown when the format has none
};
[[nodiscard]] GlBackendTargets gl_backend_targets(const GlPixelFormat& format);

}  // namespace vitreo::winapi::gl
