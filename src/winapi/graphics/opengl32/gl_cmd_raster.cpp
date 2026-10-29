// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_raster.cpp
/// @brief Rasterization state (OpenGL 1.1 spec 3.3-3.5), per-fragment state
///        (4.1), glAccum (4.2.4) and hints (5.6). Framebuffer control and
///        glClear are in gl_cmd_framebuffer.cpp.

#include "gl_cmd_internal.hpp"

namespace vitreo::winapi::gl {

namespace cmd {

namespace {

bool is_compare_func(u32 f) { return f >= GL_NEVER && f <= GL_ALWAYS; }

bool is_face(u32 face) { return face == GL_FRONT || face == GL_BACK || face == GL_FRONT_AND_BACK; }

bool is_stencil_op(u32 op) {
    return op == GL_KEEP || op == GL_ZERO || op == GL_REPLACE || op == GL_INCR || op == GL_DECR ||
           op == GL_INVERT;
}

}  // namespace

// ---- Rasterization -----------------------------------------------------------

void PointSize(GlContext& ctx, float size) {
    if (ctx.reject_inside_begin_end()) return;
    if (!(size > 0)) { ctx.record_error(GL_INVALID_VALUE); return; }   // err-zero-glPointSize
    ctx.state.point.size = size;
}

void LineWidth(GlContext& ctx, float width) {
    if (ctx.reject_inside_begin_end()) return;
    if (!(width > 0)) { ctx.record_error(GL_INVALID_VALUE); return; }  // err-zero-glLineWidth
    ctx.state.line.width = width;
}

void LineStipple(GlContext& ctx, i32 factor, u16 pattern) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.line.stipple_repeat = std::clamp(factor, 1, 256);   // spec 3.4.2
    ctx.state.line.stipple_pattern = pattern;
}

void CullFace(GlContext& ctx, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;
    if (!is_face(mode)) { ctx.record_error(GL_INVALID_ENUM); return; }
    ctx.state.polygon.cull_face_mode = mode;
}

void FrontFace(GlContext& ctx, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;
    if (mode != GL_CW && mode != GL_CCW) { ctx.record_error(GL_INVALID_ENUM); return; }
    ctx.state.polygon.front_face = mode;
}

void PolygonMode(GlContext& ctx, u32 face, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;
    if (!is_face(face) || (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL)) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    if (face != GL_BACK) ctx.state.polygon.mode[0] = mode;
    if (face != GL_FRONT) ctx.state.polygon.mode[1] = mode;
}

void PolygonOffset(GlContext& ctx, float factor, float units) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.polygon.offset_factor = factor;
    ctx.state.polygon.offset_units = units;
}

void PolygonStipple(GlContext& ctx, const std::array<u32, 32>& rows) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.polygon_stipple.rows = rows;
}

bool GetPolygonStipple(GlContext& ctx, std::array<u32, 32>& rows) {
    if (ctx.reject_inside_begin_end()) return false;
    rows = ctx.state.polygon_stipple.rows;
    return true;
}

// ---- Per-fragment operations ---------------------------------------------------

void Scissor(GlContext& ctx, i32 x, i32 y, i32 width, i32 height) {
    if (ctx.reject_inside_begin_end()) return;
    if (width < 0 || height < 0) { ctx.record_error(GL_INVALID_VALUE); return; }  // err-neg-size-glScissor
    ctx.state.scissor = GlScissorState{x, y, width, height};
}

void AlphaFunc(GlContext& ctx, u32 func, float ref) {
    if (ctx.reject_inside_begin_end()) return;
    if (!is_compare_func(func)) { ctx.record_error(GL_INVALID_ENUM); return; }
    ctx.state.color_buffer.alpha_func = func;
    ctx.state.color_buffer.alpha_ref = detail::clamp01(ref);   // spec 4.1.3
}

void StencilFunc(GlContext& ctx, u32 func, i32 ref, u32 mask) {
    if (ctx.reject_inside_begin_end()) return;
    if (!is_compare_func(func)) { ctx.record_error(GL_INVALID_ENUM); return; }
    // Spec 4.1.4: ref is clamped to [0, 2^s - 1].
    const u32 bits = ctx.framebuffer().stencil_bits;
    const i64 max_ref = bits >= 31 ? 0x7FFFFFFF : (i64{1} << bits) - 1;
    ctx.state.stencil_buffer.func = func;
    ctx.state.stencil_buffer.ref = static_cast<i32>(std::clamp<i64>(ref, 0, max_ref));
    ctx.state.stencil_buffer.value_mask = mask;
}

void StencilOp(GlContext& ctx, u32 fail, u32 zfail, u32 zpass) {
    if (ctx.reject_inside_begin_end()) return;
    if (!is_stencil_op(fail) || !is_stencil_op(zfail) || !is_stencil_op(zpass)) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    ctx.state.stencil_buffer.fail = fail;
    ctx.state.stencil_buffer.pass_depth_fail = zfail;
    ctx.state.stencil_buffer.pass_depth_pass = zpass;
}

void DepthFunc(GlContext& ctx, u32 func) {
    if (ctx.reject_inside_begin_end()) return;
    if (!is_compare_func(func)) { ctx.record_error(GL_INVALID_ENUM); return; }
    ctx.state.depth_buffer.func = func;
}

void BlendFunc(GlContext& ctx, u32 sfactor, u32 dfactor) {
    if (ctx.reject_inside_begin_end()) return;
    // Spec 4.1.6, Tables 4.1-4.2 (GL 1.1 has no constant-colour factors).
    const bool src_ok = sfactor == GL_ZERO || sfactor == GL_ONE || sfactor == GL_DST_COLOR ||
                        sfactor == GL_ONE_MINUS_DST_COLOR || sfactor == GL_SRC_ALPHA ||
                        sfactor == GL_ONE_MINUS_SRC_ALPHA || sfactor == GL_DST_ALPHA ||
                        sfactor == GL_ONE_MINUS_DST_ALPHA || sfactor == GL_SRC_ALPHA_SATURATE;
    const bool dst_ok = dfactor == GL_ZERO || dfactor == GL_ONE || dfactor == GL_SRC_COLOR ||
                        dfactor == GL_ONE_MINUS_SRC_COLOR || dfactor == GL_SRC_ALPHA ||
                        dfactor == GL_ONE_MINUS_SRC_ALPHA || dfactor == GL_DST_ALPHA ||
                        dfactor == GL_ONE_MINUS_DST_ALPHA;
    if (!src_ok || !dst_ok) { ctx.record_error(GL_INVALID_ENUM); return; }
    ctx.state.color_buffer.blend_src = sfactor;
    ctx.state.color_buffer.blend_dst = dfactor;
}

void LogicOp(GlContext& ctx, u32 opcode) {
    if (ctx.reject_inside_begin_end()) return;
    if (opcode < GL_CLEAR || opcode > GL_SET) { ctx.record_error(GL_INVALID_ENUM); return; }
    ctx.state.color_buffer.logic_op_mode = opcode;
}

void Accum(GlContext& ctx, u32 op, float value) {
    (void)value;
    if (ctx.reject_inside_begin_end()) return;
    if (op != GL_ACCUM && op != GL_LOAD && op != GL_RETURN && op != GL_MULT && op != GL_ADD) {
        ctx.record_error(GL_INVALID_ENUM);   // err-glAccum-bad-op
        return;
    }
    const GlFramebufferConfig& fb = ctx.framebuffer();
    const bool has_accum = fb.accum_red_bits || fb.accum_green_bits || fb.accum_blue_bits ||
                           fb.accum_alpha_bits;
    // Spec 4.2.4: no accumulation buffer -> INVALID_OPERATION. With one, the
    // accumulation buffer is Phase 2 (Design 11) and refuses loudly.
    if (has_accum) gl_log_unimplemented_once("glAccum", "the accumulation buffer is Phase 2");
    ctx.record_error(GL_INVALID_OPERATION);
}

// ---- Hints -------------------------------------------------------------------

void Hint(GlContext& ctx, u32 target, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;
    GlHintState& h = ctx.state.hint;
    u32* slot = nullptr;
    switch (target) {
    case GL_PERSPECTIVE_CORRECTION_HINT: slot = &h.perspective_correction; break;
    case GL_POINT_SMOOTH_HINT: slot = &h.point_smooth; break;
    case GL_LINE_SMOOTH_HINT: slot = &h.line_smooth; break;
    case GL_POLYGON_SMOOTH_HINT: slot = &h.polygon_smooth; break;
    case GL_FOG_HINT: slot = &h.fog; break;
    default: break;
    }
    if (!slot || (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE)) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    *slot = mode;
}

}  // namespace cmd

// ---- Polygon stipple bitmap transfer (spec 3.6.4 / 4.3.2, type BITMAP) -------

namespace {

/// Spec 3.6.4: k = a * ceil(l / (8a)) bytes per row, l = ROW_LENGTH or 32.
usize stipple_row_stride(const GlPixelStoreModes& m) {
    const usize l = m.row_length > 0 ? static_cast<usize>(m.row_length) : 32;
    const usize a = static_cast<usize>(m.alignment > 0 ? m.alignment : 1);
    return a * ((l + 8 * a - 1) / (8 * a));
}

}  // namespace

std::array<u32, 32> gl_unpack_stipple(const GlPixelStoreModes& m, std::span<const u8> bytes) noexcept {
    std::array<u32, 32> rows{};
    const usize k = stipple_row_stride(m);
    const usize skip_pixels = static_cast<usize>(m.skip_pixels > 0 ? m.skip_pixels : 0);
    const usize skip_rows = static_cast<usize>(m.skip_rows > 0 ? m.skip_rows : 0);
    for (usize y = 0; y < 32; ++y) {
        const usize row = (skip_rows + y) * k;
        for (usize x = 0; x < 32; ++x) {
            const usize n = skip_pixels + x;
            const usize at = row + n / 8;
            if (at >= bytes.size()) continue;
            const u32 bit = m.lsb_first ? (n % 8) : (7 - n % 8);
            if ((bytes[at] >> bit) & 1u) rows[y] |= 1u << x;
        }
    }
    return rows;
}

void gl_pack_stipple(const GlPixelStoreModes& m, const std::array<u32, 32>& rows,
                     std::span<u8> bytes) noexcept {
    const usize k = stipple_row_stride(m);
    const usize skip_pixels = static_cast<usize>(m.skip_pixels > 0 ? m.skip_pixels : 0);
    const usize skip_rows = static_cast<usize>(m.skip_rows > 0 ? m.skip_rows : 0);
    for (usize y = 0; y < 32; ++y) {
        const usize row = (skip_rows + y) * k;
        for (usize x = 0; x < 32; ++x) {
            const usize n = skip_pixels + x;
            const usize at = row + n / 8;
            if (at >= bytes.size()) continue;
            const u32 bit = m.lsb_first ? (n % 8) : (7 - n % 8);
            if ((rows[y] >> x) & 1u) bytes[at] = static_cast<u8>(bytes[at] | (1u << bit));
            else bytes[at] = static_cast<u8>(bytes[at] & ~(1u << bit));
        }
    }
}

}  // namespace vitreo::winapi::gl
