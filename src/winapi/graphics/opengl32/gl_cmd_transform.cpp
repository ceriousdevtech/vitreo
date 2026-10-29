// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_transform.cpp
/// @brief Coordinate transformation commands (OpenGL 1.1 spec 2.10-2.11).

#include "gl_cmd_internal.hpp"

namespace vitreo::winapi::gl::cmd {

using detail::plane_to_eye;

namespace {

void multiply_current(GlContext& ctx, const GlMatrix& m) {
    GlMatrix& top = ctx.current_stack().back();
    top = gl_multiply(top, m);   // spec 2.10.2: C' = C * M
}

}  // namespace

void MatrixMode(GlContext& ctx, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    ctx.state.transform.matrix_mode = mode;
}

void LoadIdentity(GlContext& ctx) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.current_stack().back() = gl_identity_matrix();
}

void LoadMatrix(GlContext& ctx, const GlMatrix& m) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.current_stack().back() = m;
}

void MultMatrix(GlContext& ctx, const GlMatrix& m) {
    if (ctx.reject_inside_begin_end()) return;
    multiply_current(ctx, m);
}

void Rotate(GlContext& ctx, double angle, double x, double y, double z) {
    if (ctx.reject_inside_begin_end()) return;
    const double len = std::sqrt(x * x + y * y + z * z);
    if (len == 0.0) return;   // no axis: the rotation is the identity
    x /= len;
    y /= len;
    z /= len;
    const double rad = angle * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    const double t = 1.0 - c;
    // Spec 2.10.2: R = u u^T + cos(theta)(I - u u^T) + sin(theta) S.
    GlMatrix r = gl_identity_matrix();
    r[0] = x * x * t + c;
    r[1] = y * x * t + z * s;
    r[2] = x * z * t - y * s;
    r[4] = x * y * t - z * s;
    r[5] = y * y * t + c;
    r[6] = y * z * t + x * s;
    r[8] = x * z * t + y * s;
    r[9] = y * z * t - x * s;
    r[10] = z * z * t + c;
    multiply_current(ctx, r);
}

void Scale(GlContext& ctx, double x, double y, double z) {
    if (ctx.reject_inside_begin_end()) return;
    GlMatrix m = gl_identity_matrix();
    m[0] = x;
    m[5] = y;
    m[10] = z;
    multiply_current(ctx, m);
}

void Translate(GlContext& ctx, double x, double y, double z) {
    if (ctx.reject_inside_begin_end()) return;
    GlMatrix m = gl_identity_matrix();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    multiply_current(ctx, m);
}

void Frustum(GlContext& ctx, double l, double r, double b, double t, double n, double f) {
    if (ctx.reject_inside_begin_end()) return;
    if (n <= 0 || f <= 0 || l == r || b == t || n == f) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    GlMatrix m{};
    m[0] = 2 * n / (r - l);
    m[5] = 2 * n / (t - b);
    m[8] = (r + l) / (r - l);
    m[9] = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1;
    m[14] = -2 * f * n / (f - n);
    multiply_current(ctx, m);
}

void Ortho(GlContext& ctx, double l, double r, double b, double t, double n, double f) {
    if (ctx.reject_inside_begin_end()) return;
    if (l == r || b == t || n == f) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    GlMatrix m = gl_identity_matrix();
    m[0] = 2 / (r - l);
    m[5] = 2 / (t - b);
    m[10] = -2 / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    multiply_current(ctx, m);
}

void PushMatrix(GlContext& ctx) {
    if (ctx.reject_inside_begin_end()) return;
    auto& stack = ctx.current_stack();
    // err-overflow-glPushMatrix-modelview: 31 pushes reach depth 32, the 32nd overflows.
    if (stack.size() >= ctx.current_stack_limit()) {
        ctx.record_error(GL_STACK_OVERFLOW);
        return;
    }
    stack.push_back(stack.back());
}

void PopMatrix(GlContext& ctx) {
    if (ctx.reject_inside_begin_end()) return;
    auto& stack = ctx.current_stack();
    if (stack.size() <= 1) {
        ctx.record_error(GL_STACK_UNDERFLOW);
        return;
    }
    stack.pop_back();
}

void Viewport(GlContext& ctx, i32 x, i32 y, i32 width, i32 height) {
    if (ctx.reject_inside_begin_end()) return;
    if (width < 0 || height < 0) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    // Spec 2.10.1: width and height are clamped to MAX_VIEWPORT_DIMS.
    GlViewportState& v = ctx.state.viewport;
    v.x = x;
    v.y = y;
    v.width = std::min(width, kMaxViewportDim);
    v.height = std::min(height, kMaxViewportDim);
}

void DepthRange(GlContext& ctx, double near_value, double far_value) {
    if (ctx.reject_inside_begin_end()) return;
    // Spec 2.10.1: n and f are clamped to [0, 1].
    ctx.state.viewport.depth_near = std::clamp(near_value, 0.0, 1.0);
    ctx.state.viewport.depth_far = std::clamp(far_value, 0.0, 1.0);
}

void ClipPlane(GlContext& ctx, u32 plane, const std::array<double, 4>& equation) {
    if (ctx.reject_inside_begin_end()) return;
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + kMaxClipPlanes) {
        ctx.record_error(GL_INVALID_ENUM);   // err-bad-plane-glClipPlane-index6
        return;
    }
    // Spec 2.11: stored in eye coordinates, p' = p M^-1.
    ctx.state.transform.clip_planes[plane - GL_CLIP_PLANE0] = plane_to_eye(ctx.modelview(), equation);
}

bool GetClipPlane(GlContext& ctx, u32 plane, std::array<double, 4>& out) {
    if (ctx.reject_inside_begin_end()) return false;
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + kMaxClipPlanes) {
        ctx.record_error(GL_INVALID_ENUM);
        return false;
    }
    out = ctx.state.transform.clip_planes[plane - GL_CLIP_PLANE0];
    return true;
}

}  // namespace vitreo::winapi::gl::cmd
