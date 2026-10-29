// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_framebuffer.cpp
/// @brief Whole-framebuffer operations (OpenGL 1.1 spec 4.2): buffer
///        selection, write masks, clear values, glClear, and glFlush /
///        glFinish (5.5). The pixels live in the context's window drawable
///        (gl_drawable.hpp); a context with none (no backend device, or a DC
///        without a window) keeps the state and draws nothing.

#include "gl_cmd_internal.hpp"
#include "gl_drawable.hpp"

namespace vitreo::winapi::gl {

namespace cmd {

namespace {

/// True when at least one named buffer exists in @p fb.
bool any_buffer_exists(const GlBufferSet& s, const GlFramebufferConfig& fb) {
    if (s.aux != 0xFFFFFFFFu) return s.aux < fb.aux_buffers;
    return s.front_left || (s.front_right && fb.stereo) || (s.back_left && fb.double_buffer) ||
           (s.back_right && fb.double_buffer && fb.stereo);
}

}  // namespace

// ---- Buffer selection (spec 4.2.1, 4.3.2) ---------------------------------------

void DrawBuffer(GlContext& ctx, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;
    if (mode != GL_NONE) {
        const auto set = gl_buffers_named(mode);
        if (!set) { ctx.record_error(GL_INVALID_ENUM); return; }   // err-bad-enum-glDrawBuffer
        // Spec 4.2.1: none of the named buffers exists -> INVALID_OPERATION.
        if (!any_buffer_exists(*set, ctx.framebuffer())) { ctx.record_error(GL_INVALID_OPERATION); return; }
    }
    // Design 1: a draw-buffer change ends the open pass (it targets the old buffers).
    if (mode != ctx.state.color_buffer.draw_buffer && ctx.drawable) ctx.drawable->end_pass();
    ctx.state.color_buffer.draw_buffer = mode;
}

void ReadBuffer(GlContext& ctx, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;
    // Spec 4.3.2: NONE and FRONT_AND_BACK are not read sources.
    const auto set = mode == GL_FRONT_AND_BACK ? std::nullopt : gl_buffers_named(mode);
    if (!set) { ctx.record_error(GL_INVALID_ENUM); return; }   // err-bad-enum-glReadBuffer
    if (!any_buffer_exists(*set, ctx.framebuffer())) { ctx.record_error(GL_INVALID_OPERATION); return; }
    ctx.state.pixel_mode.read_buffer = mode;
}

// ---- Write masks (spec 4.2.2) -----------------------------------------------------

void ColorMask(GlContext& ctx, bool r, bool g, bool b, bool a) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.color_buffer.color_writemask = {r, g, b, a};
}

void DepthMask(GlContext& ctx, bool flag) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.depth_buffer.writemask = flag;
}

void StencilMask(GlContext& ctx, u32 mask) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.stencil_buffer.writemask = mask;
}

void IndexMask(GlContext& ctx, u32 mask) {
    if (ctx.reject_inside_begin_end()) return;
    // Colour-index rendering is Phase 2 (Design 11); no format has index
    // bitplanes, so the mask is state only.
    ctx.state.color_buffer.index_writemask = mask;
}

// ---- Clear values (spec 4.2.3) ----------------------------------------------------

void ClearColor(GlContext& ctx, float r, float g, float b, float a) {
    if (ctx.reject_inside_begin_end()) return;
    // Spec 4.2.3: clamped to [0, 1].
    ctx.state.color_buffer.clear_color = {detail::clamp01(r), detail::clamp01(g), detail::clamp01(b),
                                          detail::clamp01(a)};
}

void ClearIndex(GlContext& ctx, float c) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.color_buffer.clear_index = c;
}

void ClearDepth(GlContext& ctx, double depth) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.depth_buffer.clear_value = std::clamp(depth, 0.0, 1.0);   // spec 4.2.3
}

void ClearStencil(GlContext& ctx, i32 s) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.stencil_buffer.clear_value = s;
}

void ClearAccum(GlContext& ctx, float r, float g, float b, float a) {
    if (ctx.reject_inside_begin_end()) return;
    // Spec 4.2.3: clamped to [-1, 1].
    auto c = [](float v) { return std::clamp(v, -1.0f, 1.0f); };
    ctx.state.accum_buffer.clear_value = {c(r), c(g), c(b), c(a)};
}

// ---- glClear (spec 4.2.3) ---------------------------------------------------------

void Clear(GlContext& ctx, u32 mask) {
    if (ctx.reject_inside_begin_end()) return;
    constexpr u32 kBits = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT |
                          GL_ACCUM_BUFFER_BIT;
    if ((mask & ~kBits) != 0) { ctx.record_error(GL_INVALID_VALUE); return; }
    if (!ctx.drawable) return;
    // The pixel ownership test, the scissor test, dithering and the write
    // masks apply (4.2.3); dithering is a no-op for 8-bit channels written
    // from exact clear values. No pixel format has an accumulation buffer, so
    // GL_ACCUM_BUFFER_BIT clears nothing.
    const GlState& s = ctx.state;
    GlClearParams p;
    p.color = (mask & GL_COLOR_BUFFER_BIT) != 0;
    p.depth = (mask & GL_DEPTH_BUFFER_BIT) != 0;
    p.stencil = (mask & GL_STENCIL_BUFFER_BIT) != 0;
    p.draw_buffer = s.color_buffer.draw_buffer;
    p.color_value = s.color_buffer.clear_color;
    p.color_mask = s.color_buffer.color_writemask;
    p.depth_value = static_cast<float>(s.depth_buffer.clear_value);
    p.depth_mask = s.depth_buffer.writemask;
    p.stencil_value = static_cast<u32>(s.stencil_buffer.clear_value);
    p.stencil_mask = s.stencil_buffer.writemask;
    p.scissor = s.enabled(GlCap::ScissorTest);
    p.scissor_box = {s.scissor.x, s.scissor.y, s.scissor.width, s.scissor.height};
    ctx.drawable->clear(p);
}

// ---- glFlush / glFinish (spec 5.5) ------------------------------------------------

void Flush(GlContext& ctx) {
    if (ctx.reject_inside_begin_end()) return;
    if (ctx.drawable) ctx.drawable->flush();
}

void Finish(GlContext& ctx) {
    if (ctx.reject_inside_begin_end()) return;
    if (ctx.drawable) ctx.drawable->finish();
}

}  // namespace cmd

}  // namespace vitreo::winapi::gl
