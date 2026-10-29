// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_special.cpp
/// @brief Chapter 5 "Special Functions" state: evaluator maps and grids (5.1),
///        selection (5.2), feedback (5.3) and the display-list base (5.4).
///
/// Evaluation and the SELECT/FEEDBACK render modes are Phase 2 (Design 11):
/// every entry point validates and keeps its queryable state exactly as the
/// spec says, and only the operation that would need them refuses, with a
/// one-time UNIMPLEMENTED log and GL_INVALID_OPERATION.

#include "gl_cmd_internal.hpp"

namespace vitreo::winapi::gl::cmd {

// ---- Evaluators (spec 5.1, Table 5.1) -----------------------------------------

u32 gl_map_components(u32 target) noexcept {
    static constexpr u32 kComponents[9] = {4, 1, 3, 1, 2, 3, 4, 3, 4};
    if (target >= GL_MAP1_COLOR_4 && target <= GL_MAP1_VERTEX_4) return kComponents[target - GL_MAP1_COLOR_4];
    if (target >= GL_MAP2_COLOR_4 && target <= GL_MAP2_VERTEX_4) return kComponents[target - GL_MAP2_COLOR_4];
    return 0;
}

namespace {
bool is_map1(u32 t) { return t >= GL_MAP1_COLOR_4 && t <= GL_MAP1_VERTEX_4; }
bool is_map2(u32 t) { return t >= GL_MAP2_COLOR_4 && t <= GL_MAP2_VERTEX_4; }

bool any_map_enabled(const GlContext& ctx, GlCap first) {
    for (u32 i = 0; i < 9; ++i) {
        if (ctx.state.enabled(gl_cap_offset(first, i))) return true;
    }
    return false;
}

constexpr std::string_view kEvalPhase2 = "evaluators are Phase 2";
}  // namespace

void Map1(GlContext& ctx, u32 target, double u1, double u2, i32 stride, i32 order,
          std::span<const float> points) {
    if (ctx.reject_inside_begin_end()) return;
    if (!is_map1(target)) { ctx.record_error(GL_INVALID_ENUM); return; }
    const u32 k = gl_map_components(target);
    if (u1 == u2 || stride < static_cast<i32>(k) || order < 1 || order > static_cast<i32>(kMaxEvalOrder)) {
        ctx.record_error(GL_INVALID_VALUE);   // err-bad-value-glMap1f-order0 / -domain-equal
        return;
    }
    if (points.size() < static_cast<usize>(order) * k) return;   // unreadable client memory
    GlEvalMap& m = ctx.map1[target - GL_MAP1_COLOR_4];
    m.uorder = order;
    m.vorder = 1;
    m.u1 = static_cast<float>(u1);
    m.u2 = static_cast<float>(u2);
    m.points.assign(points.data(), points.data() + static_cast<usize>(order) * k);
}

void Map2(GlContext& ctx, u32 target, double u1, double u2, i32 ustride, i32 uorder, double v1, double v2,
          i32 vstride, i32 vorder, std::span<const float> points) {
    if (ctx.reject_inside_begin_end()) return;
    if (!is_map2(target)) { ctx.record_error(GL_INVALID_ENUM); return; }
    const i32 k = static_cast<i32>(gl_map_components(target));
    const i32 max_order = static_cast<i32>(kMaxEvalOrder);
    if (u1 == u2 || v1 == v2 || ustride < k || vstride < k || uorder < 1 || uorder > max_order ||
        vorder < 1 || vorder > max_order) {
        ctx.record_error(GL_INVALID_VALUE);   // err-glMap2f-bad-order
        return;
    }
    const usize count = static_cast<usize>(uorder) * static_cast<usize>(vorder) * static_cast<usize>(k);
    if (points.size() < count) return;
    GlEvalMap& m = ctx.map2[target - GL_MAP2_COLOR_4];
    m.uorder = uorder;
    m.vorder = vorder;
    m.u1 = static_cast<float>(u1);
    m.u2 = static_cast<float>(u2);
    m.v1 = static_cast<float>(v1);
    m.v2 = static_cast<float>(v2);
    m.points.assign(points.data(), points.data() + count);
}

void MapGrid1(GlContext& ctx, i32 un, double u1, double u2) {
    if (ctx.reject_inside_begin_end()) return;
    if (un <= 0) { ctx.record_error(GL_INVALID_VALUE); return; }   // err-bad-value-glMapGrid1f-zero
    ctx.state.eval.map1_grid_segments = un;
    ctx.state.eval.map1_grid_domain = {static_cast<float>(u1), static_cast<float>(u2)};
}

void MapGrid2(GlContext& ctx, i32 un, double u1, double u2, i32 vn, double v1, double v2) {
    if (ctx.reject_inside_begin_end()) return;
    if (un <= 0 || vn <= 0) { ctx.record_error(GL_INVALID_VALUE); return; }   // err-glMapGrid2f-neg
    ctx.state.eval.map2_grid_segments = {un, vn};
    ctx.state.eval.map2_grid_domain = {static_cast<float>(u1), static_cast<float>(u2), static_cast<float>(v1),
                                       static_cast<float>(v2)};
}

bool GetMap(GlContext& ctx, u32 target, u32 query, std::vector<double>& out, GlQueryKind& kind) {
    if (ctx.reject_inside_begin_end()) return false;
    const GlEvalMap* m = nullptr;
    bool two_d = false;
    if (is_map1(target)) {
        m = &ctx.map1[target - GL_MAP1_COLOR_4];
    } else if (is_map2(target)) {
        m = &ctx.map2[target - GL_MAP2_COLOR_4];
        two_d = true;
    } else {
        ctx.record_error(GL_INVALID_ENUM);
        return false;
    }
    out.clear();
    switch (query) {
    case GL_ORDER:
        kind = GlQueryKind::Integer;
        out.push_back(m->uorder);
        if (two_d) out.push_back(m->vorder);
        return true;
    case GL_DOMAIN:
        kind = GlQueryKind::Float;
        out = {m->u1, m->u2};
        if (two_d) {
            out.push_back(m->v1);
            out.push_back(m->v2);
        }
        return true;
    case GL_COEFF:
        kind = GlQueryKind::Float;
        out.assign(m->points.begin(), m->points.end());
        return true;
    default: ctx.record_error(GL_INVALID_ENUM); return false;
    }
}

// Evaluation (spec 5.1). With no map of the dimension enabled the commands
// generate nothing, which is complete; with one enabled they would evaluate
// it, which is Phase 2. EvalCoord/EvalPoint are allowed inside Begin/End.

void EvalCoord1(GlContext& ctx, double u) {
    (void)u;
    if (!any_map_enabled(ctx, GlCap::Map1Color4)) return;   // err-glEvalCoord-outside-glBegin
    gl_log_unimplemented_once("glEvalCoord1", kEvalPhase2);
    ctx.record_error(GL_INVALID_OPERATION);
}

void EvalCoord2(GlContext& ctx, double u, double v) {
    (void)u;
    (void)v;
    if (!any_map_enabled(ctx, GlCap::Map2Color4)) return;
    gl_log_unimplemented_once("glEvalCoord2", kEvalPhase2);
    ctx.record_error(GL_INVALID_OPERATION);
}

void EvalPoint1(GlContext& ctx, i32 i) {
    (void)i;
    if (!any_map_enabled(ctx, GlCap::Map1Color4)) return;
    gl_log_unimplemented_once("glEvalPoint1", kEvalPhase2);
    ctx.record_error(GL_INVALID_OPERATION);
}

void EvalPoint2(GlContext& ctx, i32 i, i32 j) {
    (void)i;
    (void)j;
    if (!any_map_enabled(ctx, GlCap::Map2Color4)) return;
    gl_log_unimplemented_once("glEvalPoint2", kEvalPhase2);
    ctx.record_error(GL_INVALID_OPERATION);
}

void EvalMesh1(GlContext& ctx, u32 mode, i32 i1, i32 i2) {
    (void)i1;
    (void)i2;
    if (ctx.reject_inside_begin_end()) return;
    if (mode != GL_POINT && mode != GL_LINE) { ctx.record_error(GL_INVALID_ENUM); return; }  // err-glEvalMesh1-bad-mode
    if (!any_map_enabled(ctx, GlCap::Map1Color4)) return;
    gl_log_unimplemented_once("glEvalMesh1", kEvalPhase2);
    ctx.record_error(GL_INVALID_OPERATION);
}

void EvalMesh2(GlContext& ctx, u32 mode, i32 i1, i32 i2, i32 j1, i32 j2) {
    (void)i1;
    (void)i2;
    (void)j1;
    (void)j2;
    if (ctx.reject_inside_begin_end()) return;
    // A bad mode is silently ignored on Windows (err-glEvalMesh2-bad-mode: GL_NO_ERROR).
    if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) return;
    if (!any_map_enabled(ctx, GlCap::Map2Color4)) return;
    gl_log_unimplemented_once("glEvalMesh2", kEvalPhase2);
    ctx.record_error(GL_INVALID_OPERATION);
}

// ---- Selection and feedback (spec 5.2-5.3) ----------------------------------------

i32 RenderMode(GlContext& ctx, u32 mode) {
    if (ctx.reject_inside_begin_end()) return 0;
    if (mode != GL_RENDER && mode != GL_SELECT && mode != GL_FEEDBACK) {
        ctx.record_error(GL_INVALID_ENUM);   // err-glRenderMode-bad-enum
        return 0;
    }
    if (mode != GL_RENDER) {
        // Design 11: selection and feedback are Phase 2.
        gl_log_unimplemented_once("glRenderMode", "GL_SELECT and GL_FEEDBACK are Phase 2");
        ctx.record_error(GL_INVALID_OPERATION);
        return 0;
    }
    // Leaving RENDER mode for RENDER returns 0 (glRenderMode-render-to-render ret=0).
    ctx.render_mode = GL_RENDER;
    return 0;
}

void SelectBuffer(GlContext& ctx, i32 size, GuestAddr buffer) {
    if (ctx.reject_inside_begin_end()) return;
    if (size < 0) { ctx.record_error(GL_INVALID_VALUE); return; }   // err-bad-value-glSelectBuffer-neg
    if (ctx.render_mode == GL_SELECT) { ctx.record_error(GL_INVALID_OPERATION); return; }
    ctx.selection_buffer = buffer;
    ctx.selection_buffer_size = size;
}

void FeedbackBuffer(GlContext& ctx, i32 size, u32 type, GuestAddr buffer) {
    if (ctx.reject_inside_begin_end()) return;
    if (type != GL_2D && type != GL_3D && type != GL_3D_COLOR && type != GL_3D_COLOR_TEXTURE &&
        type != GL_4D_COLOR_TEXTURE) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    if (size < 0) { ctx.record_error(GL_INVALID_VALUE); return; }   // err-bad-value-glFeedbackBuffer-neg
    if (ctx.render_mode == GL_FEEDBACK) { ctx.record_error(GL_INVALID_OPERATION); return; }
    ctx.feedback_buffer = buffer;
    ctx.feedback_buffer_size = size;
    ctx.feedback_buffer_type = type;
}

// Spec 5.2: name-stack commands outside selection mode have no effect
// (err-glPushName-render-mode / -glPopName- / -glLoadName-: GL_NO_ERROR).

void InitNames(GlContext& ctx) {
    if (ctx.reject_inside_begin_end()) return;
    if (ctx.render_mode != GL_SELECT) return;
    ctx.name_stack.clear();
}

void LoadName(GlContext& ctx, u32 name) {
    if (ctx.reject_inside_begin_end()) return;
    if (ctx.render_mode != GL_SELECT) return;
    if (ctx.name_stack.empty()) { ctx.record_error(GL_INVALID_OPERATION); return; }
    ctx.name_stack.back() = name;
}

void PushName(GlContext& ctx, u32 name) {
    if (ctx.reject_inside_begin_end()) return;
    if (ctx.render_mode != GL_SELECT) return;
    if (ctx.name_stack.size() >= kMaxNameStackDepth) { ctx.record_error(GL_STACK_OVERFLOW); return; }
    ctx.name_stack.push_back(name);
}

void PopName(GlContext& ctx) {
    if (ctx.reject_inside_begin_end()) return;
    if (ctx.render_mode != GL_SELECT) return;
    if (ctx.name_stack.empty()) { ctx.record_error(GL_STACK_UNDERFLOW); return; }
    ctx.name_stack.pop_back();
}

void PassThrough(GlContext& ctx, float token) {
    (void)token;
    if (ctx.reject_inside_begin_end()) return;
    // Spec 5.3: no effect outside feedback mode.
}

// ---- Display lists (spec 5.4) ------------------------------------------------------

void ListBase(GlContext& ctx, u32 base) {
    if (ctx.reject_inside_begin_end()) return;
    ctx.state.list.base = base;
}

}  // namespace vitreo::winapi::gl::cmd
