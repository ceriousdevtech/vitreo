// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_vertex_state.cpp
/// @brief Current vertex attributes (OpenGL 1.1 spec 2.6.2, 2.7) and the
///        client vertex-array state (2.8). Drawing from the arrays, glBegin /
///        glEnd and glVertex are Task 7; these commands only set state, so
///        Task 7's assembler snapshots exactly what they leave behind.

#include "gl_cmd_internal.hpp"

namespace vitreo::winapi::gl::cmd {

// ---- Current values: all allowed between Begin and End (spec 2.6.3) ----------

void Color(GlContext& ctx, float r, float g, float b, float a) {
    ctx.state.current.color = {r, g, b, a};
    detail::apply_color_material(ctx);   // spec 2.13.3
}

void Index(GlContext& ctx, float index) { ctx.state.current.index = index; }

void Normal(GlContext& ctx, float x, float y, float z) { ctx.state.current.normal = {x, y, z}; }

void TexCoord(GlContext& ctx, float s, float t, float r, float q) {
    ctx.state.current.tex_coords = {s, t, r, q};
}

void EdgeFlag(GlContext& ctx, bool flag) { ctx.state.current.edge_flag = flag; }

// ---- Client arrays (spec 2.8, Table 2.4) ---------------------------------------
// Spec 2.6.3: these may not run between Begin and End; Vitreo reports it.

namespace {

GlClientArray* client_array(GlContext& ctx, u32 array) {
    GlVertexArrayState& va = ctx.state.vertex_array;
    switch (array) {
    case GL_VERTEX_ARRAY: return &va.vertex;
    case GL_NORMAL_ARRAY: return &va.normal;
    case GL_COLOR_ARRAY: return &va.color;
    case GL_INDEX_ARRAY: return &va.index;
    case GL_TEXTURE_COORD_ARRAY: return &va.tex_coord;
    case GL_EDGE_FLAG_ARRAY: return &va.edge_flag;
    default: return nullptr;
    }
}

void set_client_state(GlContext& ctx, u32 array, bool on) {
    if (ctx.reject_inside_begin_end()) return;
    // An unknown array is silently ignored on Windows
    // (err-bad-cap-glEnableClientState: GL_NO_ERROR).
    if (GlClientArray* a = client_array(ctx, array)) a->enabled = on;
}

bool set_pointer(GlContext& ctx, GlClientArray& a, bool size_ok, bool type_ok, i32 size, u32 type,
                 i32 stride, GuestAddr pointer) {
    if (ctx.reject_inside_begin_end()) return false;
    if (!size_ok) { ctx.record_error(GL_INVALID_VALUE); return false; }     // err-glVertexPointer-bad-size
    if (!type_ok) { ctx.record_error(GL_INVALID_ENUM); return false; }      // err-glVertexPointer-bad-type
    if (stride < 0) { ctx.record_error(GL_INVALID_VALUE); return false; }   // err-glVertexPointer-neg-stride
    a.size = size;
    a.type = type;
    a.stride = stride;
    a.pointer = pointer;
    return true;
}

}  // namespace

void EnableClientState(GlContext& ctx, u32 array) { set_client_state(ctx, array, true); }

void DisableClientState(GlContext& ctx, u32 array) { set_client_state(ctx, array, false); }

void VertexPointer(GlContext& ctx, i32 size, u32 type, i32 stride, GuestAddr pointer) {
    const bool type_ok = type == GL_SHORT || type == GL_INT || type == GL_FLOAT || type == GL_DOUBLE;
    set_pointer(ctx, ctx.state.vertex_array.vertex, size >= 2 && size <= 4, type_ok, size, type, stride, pointer);
}

void NormalPointer(GlContext& ctx, u32 type, i32 stride, GuestAddr pointer) {
    const bool type_ok = type == GL_BYTE || type == GL_SHORT || type == GL_INT || type == GL_FLOAT ||
                         type == GL_DOUBLE;
    set_pointer(ctx, ctx.state.vertex_array.normal, true, type_ok, 3, type, stride, pointer);
}

void ColorPointer(GlContext& ctx, i32 size, u32 type, i32 stride, GuestAddr pointer) {
    const bool type_ok = type == GL_BYTE || type == GL_UNSIGNED_BYTE || type == GL_SHORT ||
                         type == GL_UNSIGNED_SHORT || type == GL_INT || type == GL_UNSIGNED_INT ||
                         type == GL_FLOAT || type == GL_DOUBLE;
    set_pointer(ctx, ctx.state.vertex_array.color, size == 3 || size == 4, type_ok, size, type, stride, pointer);
}

void IndexPointer(GlContext& ctx, u32 type, i32 stride, GuestAddr pointer) {
    const bool type_ok = type == GL_UNSIGNED_BYTE || type == GL_SHORT || type == GL_INT ||
                         type == GL_FLOAT || type == GL_DOUBLE;
    set_pointer(ctx, ctx.state.vertex_array.index, true, type_ok, 1, type, stride, pointer);
}

void TexCoordPointer(GlContext& ctx, i32 size, u32 type, i32 stride, GuestAddr pointer) {
    const bool type_ok = type == GL_SHORT || type == GL_INT || type == GL_FLOAT || type == GL_DOUBLE;
    set_pointer(ctx, ctx.state.vertex_array.tex_coord, size >= 1 && size <= 4, type_ok, size, type, stride,
                pointer);
}

void EdgeFlagPointer(GlContext& ctx, i32 stride, GuestAddr pointer) {
    GlClientArray& a = ctx.state.vertex_array.edge_flag;
    set_pointer(ctx, a, true, true, 1, a.type, stride, pointer);
}

void InterleavedArrays(GlContext& ctx, u32 format, i32 stride, GuestAddr pointer) {
    if (ctx.reject_inside_begin_end()) return;
    // Spec 2.8, Table 2.5. f = sizeof(float), c = 4 ubytes rounded up to f.
    constexpr i32 f = 4;
    constexpr i32 c = 4;
    struct Layout {
        u32 format;
        bool et, ec, en;
        i32 st, sc, sv;
        u32 tc;
        i32 pc, pn, pv, s;
    };
    static constexpr Layout kLayouts[] = {
        {GL_V2F, false, false, false, 0, 0, 2, 0, 0, 0, 0, 2 * f},
        {GL_V3F, false, false, false, 0, 0, 3, 0, 0, 0, 0, 3 * f},
        {GL_C4UB_V2F, false, true, false, 0, 4, 2, GL_UNSIGNED_BYTE, 0, 0, c, c + 2 * f},
        {GL_C4UB_V3F, false, true, false, 0, 4, 3, GL_UNSIGNED_BYTE, 0, 0, c, c + 3 * f},
        {GL_C3F_V3F, false, true, false, 0, 3, 3, GL_FLOAT, 0, 0, 3 * f, 6 * f},
        {GL_N3F_V3F, false, false, true, 0, 0, 3, 0, 0, 0, 3 * f, 6 * f},
        {GL_C4F_N3F_V3F, false, true, true, 0, 4, 3, GL_FLOAT, 0, 4 * f, 7 * f, 10 * f},
        {GL_T2F_V3F, true, false, false, 2, 0, 3, 0, 0, 0, 2 * f, 5 * f},
        {GL_T4F_V4F, true, false, false, 4, 0, 4, 0, 0, 0, 4 * f, 8 * f},
        {GL_T2F_C4UB_V3F, true, true, false, 2, 4, 3, GL_UNSIGNED_BYTE, 2 * f, 0, c + 2 * f, c + 5 * f},
        {GL_T2F_C3F_V3F, true, true, false, 2, 3, 3, GL_FLOAT, 2 * f, 0, 5 * f, 8 * f},
        {GL_T2F_N3F_V3F, true, false, true, 2, 0, 3, 0, 0, 2 * f, 5 * f, 8 * f},
        {GL_T2F_C4F_N3F_V3F, true, true, true, 2, 4, 3, GL_FLOAT, 2 * f, 6 * f, 9 * f, 12 * f},
        {GL_T4F_C4F_N3F_V4F, true, true, true, 4, 4, 4, GL_FLOAT, 4 * f, 8 * f, 11 * f, 15 * f},
    };
    const Layout* layout = nullptr;
    for (const Layout& l : kLayouts) {
        if (l.format == format) layout = &l;
    }
    if (!layout) { ctx.record_error(GL_INVALID_ENUM); return; }   // err-glInterleavedArrays-bad-format
    if (stride < 0) { ctx.record_error(GL_INVALID_VALUE); return; }
    const i32 str = stride == 0 ? layout->s : stride;
    GlVertexArrayState& va = ctx.state.vertex_array;
    va.edge_flag.enabled = false;
    va.index.enabled = false;
    va.tex_coord.enabled = layout->et;
    if (layout->et) TexCoordPointer(ctx, layout->st, GL_FLOAT, str, pointer);
    va.color.enabled = layout->ec;
    if (layout->ec) ColorPointer(ctx, layout->sc, layout->tc, str, pointer + static_cast<GuestAddr>(layout->pc));
    va.normal.enabled = layout->en;
    if (layout->en) NormalPointer(ctx, GL_FLOAT, str, pointer + static_cast<GuestAddr>(layout->pn));
    va.vertex.enabled = true;
    VertexPointer(ctx, layout->sv, GL_FLOAT, str, pointer + static_cast<GuestAddr>(layout->pv));
}

}  // namespace vitreo::winapi::gl::cmd
