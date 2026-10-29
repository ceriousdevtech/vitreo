// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_context.cpp
/// @brief GlContext: error flag, begin/end gate, matrix helpers, the current
///        context, and the attribute stacks (spec chapter 6).

#include "gl_context.hpp"
#include "gl_commands.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

namespace vitreo::winapi::gl {

// ---- Matrices ----------------------------------------------------------------

GlMatrix gl_identity_matrix() noexcept {
    return GlMatrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

GlMatrix gl_multiply(const GlMatrix& a, const GlMatrix& b) noexcept {
    GlMatrix r{};
    for (u32 col = 0; col < 4; ++col) {
        for (u32 row = 0; row < 4; ++row) {
            double sum = 0;
            for (u32 k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = sum;
        }
    }
    return r;
}

std::optional<GlMatrix> gl_invert(const GlMatrix& m) noexcept {
    // Cofactor expansion (column-major storage; the algebra is layout-neutral).
    GlMatrix inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0 || !std::isfinite(det)) return std::nullopt;
    for (double& x : inv) x /= det;
    return inv;
}

// ---- GlContext -----------------------------------------------------------------

GlContext::GlContext(const GlFramebufferConfig& framebuffer,
                     std::shared_ptr<GlShareGroup> group)
    : share_group(group ? std::move(group) : std::make_shared<GlShareGroup>()),
      m_framebuffer(framebuffer) {
    gl_set_initial_state(state, framebuffer);
    modelview_stack.assign(1, gl_identity_matrix());   // Table 6.7: Identity, depth 1
    projection_stack.assign(1, gl_identity_matrix());
    texture_stack.assign(1, gl_identity_matrix());
    gl_set_initial_pixel_maps(pixel_maps);
    gl_set_initial_eval_maps(map1, map2);
    default_textures[0] = gl_initial_texture_object(0);
    default_textures[0].dimensions = 1;
    default_textures[1] = gl_initial_texture_object(0);
    default_textures[1].dimensions = 2;
    proxy_textures[0] = gl_initial_texture_object(0);
    proxy_textures[0].dimensions = 1;
    proxy_textures[1] = gl_initial_texture_object(0);
    proxy_textures[1].dimensions = 2;
}

void GlContext::record_error(u32 gl_error) noexcept {
    if (m_error == GL_NO_ERROR) m_error = gl_error;
}

u32 GlContext::take_error() noexcept {
    const u32 e = m_error;
    m_error = GL_NO_ERROR;
    return e;
}

bool GlContext::reject_inside_begin_end() noexcept {
    if (!inside_begin_end()) return false;
    record_error(GL_INVALID_OPERATION);
    return true;
}

void GlContext::on_first_make_current(i32 width, i32 height) noexcept {
    if (m_has_been_current) return;
    m_has_been_current = true;
    // Spec 2.10.1 / 4.1.2: the viewport and scissor box start as the window.
    state.viewport.x = state.viewport.y = 0;
    state.viewport.width = width < kMaxViewportDim ? width : kMaxViewportDim;
    state.viewport.height = height < kMaxViewportDim ? height : kMaxViewportDim;
    state.scissor = GlScissorState{0, 0, width, height};
}

std::vector<GlMatrix>& GlContext::current_stack() noexcept {
    switch (state.transform.matrix_mode) {
    case GL_PROJECTION: return projection_stack;
    case GL_TEXTURE: return texture_stack;
    default: return modelview_stack;
    }
}

u32 GlContext::current_stack_limit() const noexcept {
    switch (state.transform.matrix_mode) {
    case GL_PROJECTION: return kMaxProjectionStackDepth;
    case GL_TEXTURE: return kMaxTextureStackDepth;
    default: return kMaxModelviewStackDepth;
    }
}

GlTextureObject* GlContext::bound_texture(u32 dimension, const GlShareGroup::Lock& held) noexcept {
    const u32 idx = dimension == 1 ? 0 : 1;
    u32& name = dimension == 1 ? state.texture.binding_1d : state.texture.binding_2d;
    std::shared_ptr<GlTextureObject>& bound = bound_objects[idx];
    if (name != 0) {
        if (bound && bound->name == name) return bound.get();
        // A binding restored by glPopAttrib: the object of that name, when it
        // is still one of this dimensionality.
        GlShareGroup::TextureMap& map = share_group->textures(held);
        const auto it = map.find(name);
        if (it != map.end() && it->second && it->second->dimensions == dimension) {
            bound = it->second;
            return bound.get();
        }
        name = 0;   // spec 3.8.8: as if BindTexture(target, 0)
    }
    bound.reset();
    return &default_textures[idx];
}

void GlShareGroup::check(const Lock& held) const noexcept {
    if (held.mutex() == &m_mutex && held.owns_lock()) return;
    std::fprintf(stderr, "[vitreo] opengl32: share-group objects accessed without the group's lock\n");
    std::abort();
}

// ---- Current context ----------------------------------------------------------

namespace {
thread_local GlContext* t_current_context = nullptr;
}  // namespace

GlContext* current_gl_context() noexcept { return t_current_context; }

void set_current_gl_context_for_thread(GlContext* ctx) noexcept { t_current_context = ctx; }

// ---- Unimplemented-path logging --------------------------------------------------

namespace {
std::mutex g_unimplemented_mutex;
std::unordered_set<std::string> g_unimplemented_logged;
}  // namespace

void gl_forget_unimplemented_logs_for_tests() {
    std::lock_guard<std::mutex> lock(g_unimplemented_mutex);
    g_unimplemented_logged.clear();
}

void gl_log_once(std::atomic<bool>& flag, const char* text) {
    if (!flag.exchange(true)) std::fprintf(stderr, "[vitreo] opengl32: %s (logged once)\n", text);
}

void gl_log_unimplemented_once(std::string_view api, std::string_view detail) {
    std::lock_guard<std::mutex> lock(g_unimplemented_mutex);
    if (!g_unimplemented_logged.emplace(api).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED opengl32!%.*s: %.*s\n",
                 static_cast<int>(api.size()), api.data(),
                 static_cast<int>(detail.size()), detail.data());
}

// ---- Attribute stacks (spec 6) -------------------------------------------------

void gl_copy_attrib_groups(GlState& to, const GlState& from, u32 mask) noexcept {
    if (mask & GL_CURRENT_BIT) to.current = from.current;
    if (mask & GL_POINT_BIT) to.point = from.point;
    if (mask & GL_LINE_BIT) to.line = from.line;
    if (mask & GL_POLYGON_BIT) to.polygon = from.polygon;
    if (mask & GL_POLYGON_STIPPLE_BIT) to.polygon_stipple = from.polygon_stipple;
    if (mask & GL_PIXEL_MODE_BIT) to.pixel_mode = from.pixel_mode;
    if (mask & GL_LIGHTING_BIT) to.lighting = from.lighting;
    if (mask & GL_FOG_BIT) to.fog = from.fog;
    if (mask & GL_DEPTH_BUFFER_BIT) to.depth_buffer = from.depth_buffer;
    if (mask & GL_ACCUM_BUFFER_BIT) to.accum_buffer = from.accum_buffer;
    if (mask & GL_STENCIL_BUFFER_BIT) to.stencil_buffer = from.stencil_buffer;
    if (mask & GL_VIEWPORT_BIT) to.viewport = from.viewport;
    if (mask & GL_TRANSFORM_BIT) to.transform = from.transform;
    if (mask & GL_COLOR_BUFFER_BIT) to.color_buffer = from.color_buffer;
    if (mask & GL_HINT_BIT) to.hint = from.hint;
    if (mask & GL_EVAL_BIT) to.eval = from.eval;
    if (mask & GL_LIST_BIT) to.list = from.list;
    if (mask & GL_TEXTURE_BIT) to.texture = from.texture;
    if (mask & GL_SCISSOR_BIT) to.scissor = from.scissor;
    // An enable belongs to GL_ENABLE_BIT and to the group of its table row.
    for (u32 i = 0; i < kGlCapCount; ++i) {
        const auto cap = static_cast<GlCap>(i);
        if ((mask & GL_ENABLE_BIT) || (mask & gl_cap_group(cap))) {
            to.enable.set(cap, from.enable.test(cap));
        }
    }
}

namespace cmd {

void PushAttrib(GlContext& ctx, u32 mask) {
    if (ctx.reject_inside_begin_end()) return;
    if (ctx.attrib_stack.size() >= kMaxAttribStackDepth) {
        ctx.record_error(GL_STACK_OVERFLOW);
        return;
    }
    // Unknown mask bits are ignored (err-glPushAttrib-bad-mask: no error).
    GlAttribEntry entry;
    entry.mask = mask;
    entry.saved = ctx.state;
    if (mask & GL_TEXTURE_BIT) {
        const GlShareGroup::Lock held = ctx.share_group->lock();
        for (u32 dim = 1; dim <= 2; ++dim) {
            entry.bound_texture_params[dim - 1] = ctx.bound_texture(dim, held)->params;
        }
    }
    ctx.attrib_stack.push_back(std::move(entry));
}

void PopAttrib(GlContext& ctx) {
    if (ctx.reject_inside_begin_end()) return;
    if (ctx.attrib_stack.empty()) {
        ctx.record_error(GL_STACK_UNDERFLOW);
        return;
    }
    GlAttribEntry entry = std::move(ctx.attrib_stack.back());
    ctx.attrib_stack.pop_back();
    gl_copy_attrib_groups(ctx.state, entry.saved, entry.mask);
    if (entry.mask & GL_TEXTURE_BIT) {
        // Spec 6: bindings and enables first, then the now-bound objects'
        // priorities, border colours, filters and wrap modes.
        const GlShareGroup::Lock held = ctx.share_group->lock();
        for (u32 dim = 1; dim <= 2; ++dim) {
            ctx.bound_texture(dim, held)->params = entry.bound_texture_params[dim - 1];
        }
    }
}

void PushClientAttrib(GlContext& ctx, u32 mask) {
    if (ctx.reject_inside_begin_end()) return;
    if (ctx.client_attrib_stack.size() >= kMaxClientAttribStackDepth) {
        ctx.record_error(GL_STACK_OVERFLOW);
        return;
    }
    GlClientAttribEntry entry;
    entry.mask = mask;
    entry.pixel_store = ctx.state.pixel_store;
    entry.vertex_array = ctx.state.vertex_array;
    ctx.client_attrib_stack.push_back(entry);
}

void PopClientAttrib(GlContext& ctx) {
    if (ctx.reject_inside_begin_end()) return;
    if (ctx.client_attrib_stack.empty()) {
        ctx.record_error(GL_STACK_UNDERFLOW);
        return;
    }
    const GlClientAttribEntry entry = ctx.client_attrib_stack.back();
    ctx.client_attrib_stack.pop_back();
    if (entry.mask & GL_CLIENT_PIXEL_STORE_BIT) ctx.state.pixel_store = entry.pixel_store;
    if (entry.mask & GL_CLIENT_VERTEX_ARRAY_BIT) ctx.state.vertex_array = entry.vertex_array;
}

}  // namespace cmd

}  // namespace vitreo::winapi::gl
