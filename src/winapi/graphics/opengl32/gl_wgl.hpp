#pragma once
/// @file gl_wgl.hpp
/// @brief WGL context objects: the handle table behind wglCreateContext, and
///        how a GL command finds the calling thread's drawable binding.
///
/// An HGLRC is an entry in Opengl32Manager's handle table holding a GlContext,
/// the pixel format it was created for and its share group. The current
/// context is thread-local (guest threads are host threads): wglMakeCurrent
/// installs it through set_current_gl_context_for_thread, so every GL export
/// reaches it through current_gl_context(). The DC it was made current on and
/// that DC's pixel format are in wgl_current_binding().
///
/// From a GL command, `current_gl_context()` is the state and
/// `current_gl_context()->drawable` the window's default framebuffer
/// (gl_drawable.hpp), which wglMakeCurrent attaches. `wgl_current_binding().hdc`
/// is the HDC passed to wglMakeCurrent and `.pixel_format` its 1-based format
/// index (gl_pixel_format() gives the format).

#include <vitreo/common.hpp>

namespace vitreo::winapi::gl {

class GlContext;

/// The HGLRC value of @p ctx (0 for a context that is not a WGL object).
[[nodiscard]] u64 wgl_handle_of(const GlContext* ctx) noexcept;

/// What wglMakeCurrent bound on the calling thread. All zero when no context
/// is current.
struct WglBinding {
    u64 hglrc{0};
    u64 hdc{0};
    i32 pixel_format{0};
};

[[nodiscard]] WglBinding wgl_current_binding() noexcept;

}  // namespace vitreo::winapi::gl
