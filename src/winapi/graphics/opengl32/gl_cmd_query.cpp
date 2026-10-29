// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_query.cpp
/// @brief glGetError, glGetString, glEnable/glDisable/glIsEnabled and the
///        simple state queries glGet{Boolean,Integer,Float,Double}v and
///        glGetPointerv (OpenGL 1.1 spec 2.5, 6.1), with spec 6.1.2's type
///        conversions and Table 2.6's integer-to-float colour conversions.

#include "gl_cmd_internal.hpp"

#include <limits>

namespace vitreo::winapi::gl {

// ---- Conversions ---------------------------------------------------------------

float gl_byte_to_float(i8 c) noexcept { return static_cast<float>((2.0 * c + 1.0) / 255.0); }
float gl_ubyte_to_float(u8 c) noexcept { return static_cast<float>(c / 255.0); }
float gl_short_to_float(i16 c) noexcept { return static_cast<float>((2.0 * c + 1.0) / 65535.0); }
float gl_ushort_to_float(u16 c) noexcept { return static_cast<float>(c / 65535.0); }
float gl_int_to_float(i32 c) noexcept { return static_cast<float>((2.0 * c + 1.0) / 4294967295.0); }
float gl_uint_to_float(u32 c) noexcept { return static_cast<float>(c / 4294967295.0); }

u8 gl_query_to_boolean(double value) noexcept { return value != 0.0 ? 1 : 0; }

i32 gl_query_to_integer(GlQueryKind kind, double value) noexcept {
    constexpr double kMax = 2147483647.0;
    constexpr double kMin = -2147483648.0;
    switch (kind) {
    case GlQueryKind::Boolean: return value != 0.0 ? 1 : 0;
    case GlQueryKind::Mask: return static_cast<i32>(static_cast<u32>(static_cast<u64>(value) & 0xFFFFFFFFu));
    case GlQueryKind::Color: {
        // Spec 6.1.2 / Table 4.7 INT: [(2^32 - 1) c - 1] / 2, rounded to the
        // nearest integer; values outside [-1, 1] are undefined and saturate.
        const double c = std::clamp(value, -1.0, 1.0);
        const double i = std::floor(((4294967295.0 * c) - 1.0) / 2.0 + 0.5);
        return static_cast<i32>(std::clamp(i, kMin, kMax));
    }
    case GlQueryKind::Float:
    case GlQueryKind::Integer:
    default: {
        // A floating-point value is rounded to the nearest integer; a value
        // too large for the type is the nearest representable one.
        if (!(value == value)) return 0;
        return static_cast<i32>(std::clamp(std::round(value), kMin, kMax));
    }
    }
}

namespace cmd {

// ---- Errors -------------------------------------------------------------------

u32 GetError(GlContext& ctx) {
    if (ctx.inside_begin_end()) {
        // glGetError is not allowed inside Begin/End: it records
        // INVALID_OPERATION and returns the pending error without clearing it
        // (err-geterror-in-glBegin: in-begin=0x0502 in-begin-again=0x0502
        // after-glEnd=0x0502 then=0).
        ctx.record_error(GL_INVALID_OPERATION);
        return ctx.pending_error();
    }
    return ctx.take_error();
}

const char* GetString(GlContext& ctx, u32 name) {
    if (ctx.reject_inside_begin_end()) return nullptr;
    // Design 10: Vitreo's own identity, GL 1.1. The extensions are GDI
    // Generic's, implemented (gl_cmd_extensions.cpp, gl_pixels.cpp): the
    // string is winoracle's, x86 and x64 (glgetstring-GL_EXTENSIONS).
    switch (name) {
    case GL_VENDOR: return "Vitreo";
    case GL_RENDERER: return "Vitreo Metal";
    case GL_VERSION: return "1.1.0";
    case GL_EXTENSIONS: return "GL_WIN_swap_hint GL_EXT_bgra GL_EXT_paletted_texture";
    default:
        ctx.record_error(GL_INVALID_ENUM);   // glgetstring-invalid-0x1234: NULL + INVALID_ENUM
        return nullptr;
    }
}

// ---- Enables -------------------------------------------------------------------

namespace {
void set_cap(GlContext& ctx, u32 cap, bool on) {
    if (ctx.reject_inside_begin_end()) return;
    const auto c = gl_cap_from_enum(cap);
    if (!c) {
        ctx.record_error(GL_INVALID_ENUM);   // err-bad-enum-glEnable / -glDisable
        return;
    }
    ctx.state.enable.set(*c, on);
    // Spec 2.13.3: once enabled the tracked material follows the current colour.
    if (*c == GlCap::ColorMaterial && on) detail::apply_color_material(ctx);
}

bool client_array_enabled(const GlContext& ctx, u32 array, bool& out) {
    const GlVertexArrayState& va = ctx.state.vertex_array;
    switch (array) {
    case GL_VERTEX_ARRAY: out = va.vertex.enabled; return true;
    case GL_NORMAL_ARRAY: out = va.normal.enabled; return true;
    case GL_COLOR_ARRAY: out = va.color.enabled; return true;
    case GL_INDEX_ARRAY: out = va.index.enabled; return true;
    case GL_TEXTURE_COORD_ARRAY: out = va.tex_coord.enabled; return true;
    case GL_EDGE_FLAG_ARRAY: out = va.edge_flag.enabled; return true;
    default: return false;
    }
}
}  // namespace

void Enable(GlContext& ctx, u32 cap) { set_cap(ctx, cap, true); }

void Disable(GlContext& ctx, u32 cap) { set_cap(ctx, cap, false); }

bool IsEnabled(GlContext& ctx, u32 cap) {
    if (ctx.reject_inside_begin_end()) return false;
    if (const auto c = gl_cap_from_enum(cap)) return ctx.state.enabled(*c);
    bool on = false;
    if (client_array_enabled(ctx, cap, on)) return on;
    ctx.record_error(GL_INVALID_ENUM);   // get-invalid-*: glIsEnabled=INVALID_ENUM
    return false;
}

// ---- glGet*v (spec Tables 6.5-6.22) ----------------------------------------------

namespace {

using K = GlQueryKind;

void put4(GlQuery& out, K kind, const GlVec4& v) { out.set(kind, {v[0], v[1], v[2], v[3]}); }

void put_matrix(GlQuery& out, const GlMatrix& m) {
    out.kind = K::Float;
    out.count = 16;
    for (u32 i = 0; i < 16; ++i) out.v[i] = m[i];
}

void put_array(GlQuery& out, u32 pname, const GlClientArray& a, u32 base_enable, u32 base_size,
               u32 base_type, u32 base_stride) {
    if (pname == base_enable) out.set(K::Boolean, {a.enabled ? 1.0 : 0.0});
    else if (pname == base_size) out.set(K::Integer, {static_cast<double>(a.size)});
    else if (pname == base_type) out.set(K::Integer, {static_cast<double>(a.type)});
    else if (pname == base_stride) out.set(K::Integer, {static_cast<double>(a.stride)});
}

}  // namespace

bool Get(GlContext& ctx, u32 pname, GlQuery& out) {
    if (ctx.reject_inside_begin_end()) return false;   // err-glGetIntegerv-inside-glBegin
    const GlState& s = ctx.state;
    const GlFramebufferConfig& fb = ctx.framebuffer();
    out = GlQuery{};

    // Every IsEnabled capability is also a boolean glGet (spec 6.1.1).
    if (const auto cap = gl_cap_from_enum(pname)) {
        out.set(K::Boolean, {s.enabled(*cap) ? 1.0 : 0.0});
        return true;
    }

    auto b = [&](bool v) { out.set(K::Boolean, {v ? 1.0 : 0.0}); };
    auto i = [&](double v) { out.set(K::Integer, {v}); };
    auto f = [&](double v) { out.set(K::Float, {v}); };

    switch (pname) {
    // Table 6.5 Current Values and Associated Data
    case GL_CURRENT_COLOR: put4(out, K::Color, s.current.color); break;
    case GL_CURRENT_INDEX: f(s.current.index); break;
    case GL_CURRENT_TEXTURE_COORDS: put4(out, K::Float, s.current.tex_coords); break;
    case GL_CURRENT_NORMAL:
        out.set(K::Color, {s.current.normal[0], s.current.normal[1], s.current.normal[2]});
        break;
    case GL_CURRENT_RASTER_POSITION: put4(out, K::Float, s.current.raster_position); break;
    case GL_CURRENT_RASTER_DISTANCE: f(s.current.raster_distance); break;
    case GL_CURRENT_RASTER_COLOR: put4(out, K::Color, s.current.raster_color); break;
    case GL_CURRENT_RASTER_INDEX: f(s.current.raster_index); break;
    case GL_CURRENT_RASTER_TEXTURE_COORDS: put4(out, K::Float, s.current.raster_tex_coords); break;
    case GL_CURRENT_RASTER_POSITION_VALID: b(s.current.raster_position_valid); break;
    case GL_EDGE_FLAG: b(s.current.edge_flag); break;

    // Table 6.6 Vertex Array Data
    case GL_VERTEX_ARRAY: case GL_VERTEX_ARRAY_SIZE: case GL_VERTEX_ARRAY_TYPE: case GL_VERTEX_ARRAY_STRIDE:
        put_array(out, pname, s.vertex_array.vertex, GL_VERTEX_ARRAY, GL_VERTEX_ARRAY_SIZE,
                  GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE);
        break;
    case GL_NORMAL_ARRAY: case GL_NORMAL_ARRAY_TYPE: case GL_NORMAL_ARRAY_STRIDE:
        put_array(out, pname, s.vertex_array.normal, GL_NORMAL_ARRAY, 0, GL_NORMAL_ARRAY_TYPE,
                  GL_NORMAL_ARRAY_STRIDE);
        break;
    case GL_COLOR_ARRAY: case GL_COLOR_ARRAY_SIZE: case GL_COLOR_ARRAY_TYPE: case GL_COLOR_ARRAY_STRIDE:
        put_array(out, pname, s.vertex_array.color, GL_COLOR_ARRAY, GL_COLOR_ARRAY_SIZE, GL_COLOR_ARRAY_TYPE,
                  GL_COLOR_ARRAY_STRIDE);
        break;
    case GL_INDEX_ARRAY: case GL_INDEX_ARRAY_TYPE: case GL_INDEX_ARRAY_STRIDE:
        put_array(out, pname, s.vertex_array.index, GL_INDEX_ARRAY, 0, GL_INDEX_ARRAY_TYPE,
                  GL_INDEX_ARRAY_STRIDE);
        break;
    case GL_TEXTURE_COORD_ARRAY: case GL_TEXTURE_COORD_ARRAY_SIZE: case GL_TEXTURE_COORD_ARRAY_TYPE:
    case GL_TEXTURE_COORD_ARRAY_STRIDE:
        put_array(out, pname, s.vertex_array.tex_coord, GL_TEXTURE_COORD_ARRAY, GL_TEXTURE_COORD_ARRAY_SIZE,
                  GL_TEXTURE_COORD_ARRAY_TYPE, GL_TEXTURE_COORD_ARRAY_STRIDE);
        break;
    case GL_EDGE_FLAG_ARRAY: case GL_EDGE_FLAG_ARRAY_STRIDE:
        put_array(out, pname, s.vertex_array.edge_flag, GL_EDGE_FLAG_ARRAY, 0, 0, GL_EDGE_FLAG_ARRAY_STRIDE);
        break;

    // Table 6.7 Transformation state
    case GL_MODELVIEW_MATRIX: put_matrix(out, ctx.modelview()); break;
    case GL_PROJECTION_MATRIX: put_matrix(out, ctx.projection()); break;
    case GL_TEXTURE_MATRIX: put_matrix(out, ctx.texture_matrix()); break;
    case GL_VIEWPORT:
        out.set(K::Integer, {static_cast<double>(s.viewport.x), static_cast<double>(s.viewport.y),
                             static_cast<double>(s.viewport.width), static_cast<double>(s.viewport.height)});
        break;
    case GL_DEPTH_RANGE: out.set(K::Color, {s.viewport.depth_near, s.viewport.depth_far}); break;
    case GL_MODELVIEW_STACK_DEPTH: i(static_cast<double>(ctx.modelview_stack.size())); break;
    case GL_PROJECTION_STACK_DEPTH: i(static_cast<double>(ctx.projection_stack.size())); break;
    case GL_TEXTURE_STACK_DEPTH: i(static_cast<double>(ctx.texture_stack.size())); break;
    case GL_MATRIX_MODE: i(s.transform.matrix_mode); break;

    // Table 6.8 Coloring
    case GL_FOG_COLOR: put4(out, K::Color, s.fog.color); break;
    case GL_FOG_INDEX: f(s.fog.index); break;
    case GL_FOG_DENSITY: f(s.fog.density); break;
    case GL_FOG_START: f(s.fog.start); break;
    case GL_FOG_END: f(s.fog.end); break;
    case GL_FOG_MODE: i(s.fog.mode); break;
    case GL_SHADE_MODEL: i(s.lighting.shade_model); break;

    // Table 6.9 Lighting
    case GL_COLOR_MATERIAL_PARAMETER: i(s.lighting.color_material_parameter); break;
    case GL_COLOR_MATERIAL_FACE: i(s.lighting.color_material_face); break;
    case GL_LIGHT_MODEL_AMBIENT: put4(out, K::Color, s.lighting.model_ambient); break;
    case GL_LIGHT_MODEL_LOCAL_VIEWER: b(s.lighting.model_local_viewer); break;
    case GL_LIGHT_MODEL_TWO_SIDE: b(s.lighting.model_two_side); break;

    // Table 6.10 Rasterization
    case GL_POINT_SIZE: f(s.point.size); break;
    case GL_LINE_WIDTH: f(s.line.width); break;
    case GL_LINE_STIPPLE_PATTERN: i(s.line.stipple_pattern); break;
    case GL_LINE_STIPPLE_REPEAT: i(s.line.stipple_repeat); break;
    case GL_CULL_FACE_MODE: i(s.polygon.cull_face_mode); break;
    case GL_FRONT_FACE: i(s.polygon.front_face); break;
    case GL_POLYGON_MODE:
        out.set(K::Integer, {static_cast<double>(s.polygon.mode[0]), static_cast<double>(s.polygon.mode[1])});
        break;
    case GL_POLYGON_OFFSET_FACTOR: f(s.polygon.offset_factor); break;
    case GL_POLYGON_OFFSET_UNITS: f(s.polygon.offset_units); break;

    // Table 6.11 Texture Objects
    case GL_TEXTURE_BINDING_1D: i(s.texture.binding_1d); break;
    case GL_TEXTURE_BINDING_2D: i(s.texture.binding_2d); break;

    // Table 6.13 Pixel Operations
    case GL_SCISSOR_BOX:
        out.set(K::Integer, {static_cast<double>(s.scissor.x), static_cast<double>(s.scissor.y),
                             static_cast<double>(s.scissor.width), static_cast<double>(s.scissor.height)});
        break;
    case GL_ALPHA_TEST_FUNC: i(s.color_buffer.alpha_func); break;
    case GL_ALPHA_TEST_REF: f(s.color_buffer.alpha_ref); break;
    case GL_STENCIL_FUNC: i(s.stencil_buffer.func); break;
    case GL_STENCIL_VALUE_MASK: out.set(K::Mask, {static_cast<double>(s.stencil_buffer.value_mask)}); break;
    case GL_STENCIL_REF: i(s.stencil_buffer.ref); break;
    case GL_STENCIL_FAIL: i(s.stencil_buffer.fail); break;
    case GL_STENCIL_PASS_DEPTH_FAIL: i(s.stencil_buffer.pass_depth_fail); break;
    case GL_STENCIL_PASS_DEPTH_PASS: i(s.stencil_buffer.pass_depth_pass); break;
    case GL_DEPTH_FUNC: i(s.depth_buffer.func); break;
    case GL_BLEND_SRC: i(s.color_buffer.blend_src); break;
    case GL_BLEND_DST: i(s.color_buffer.blend_dst); break;
    case GL_LOGIC_OP_MODE: i(s.color_buffer.logic_op_mode); break;

    // Table 6.14 Framebuffer Control
    case GL_DRAW_BUFFER: i(s.color_buffer.draw_buffer); break;
    case GL_INDEX_WRITEMASK: out.set(K::Mask, {static_cast<double>(s.color_buffer.index_writemask)}); break;
    case GL_COLOR_WRITEMASK: {
        const auto& m = s.color_buffer.color_writemask;
        out.set(K::Boolean, {m[0] ? 1.0 : 0.0, m[1] ? 1.0 : 0.0, m[2] ? 1.0 : 0.0, m[3] ? 1.0 : 0.0});
        break;
    }
    case GL_DEPTH_WRITEMASK: b(s.depth_buffer.writemask); break;
    case GL_STENCIL_WRITEMASK: out.set(K::Mask, {static_cast<double>(s.stencil_buffer.writemask)}); break;
    case GL_COLOR_CLEAR_VALUE: put4(out, K::Color, s.color_buffer.clear_color); break;
    case GL_INDEX_CLEAR_VALUE: f(s.color_buffer.clear_index); break;
    case GL_DEPTH_CLEAR_VALUE: out.set(K::Color, {s.depth_buffer.clear_value}); break;
    case GL_STENCIL_CLEAR_VALUE: i(s.stencil_buffer.clear_value); break;
    case GL_ACCUM_CLEAR_VALUE: put4(out, K::Color, s.accum_buffer.clear_value); break;

    // Table 6.15 Pixels (pixel store)
    case GL_UNPACK_SWAP_BYTES: b(s.pixel_store.unpack.swap_bytes); break;
    case GL_UNPACK_LSB_FIRST: b(s.pixel_store.unpack.lsb_first); break;
    case GL_UNPACK_ROW_LENGTH: i(s.pixel_store.unpack.row_length); break;
    case GL_UNPACK_SKIP_ROWS: i(s.pixel_store.unpack.skip_rows); break;
    case GL_UNPACK_SKIP_PIXELS: i(s.pixel_store.unpack.skip_pixels); break;
    case GL_UNPACK_ALIGNMENT: i(s.pixel_store.unpack.alignment); break;
    case GL_PACK_SWAP_BYTES: b(s.pixel_store.pack.swap_bytes); break;
    case GL_PACK_LSB_FIRST: b(s.pixel_store.pack.lsb_first); break;
    case GL_PACK_ROW_LENGTH: i(s.pixel_store.pack.row_length); break;
    case GL_PACK_SKIP_ROWS: i(s.pixel_store.pack.skip_rows); break;
    case GL_PACK_SKIP_PIXELS: i(s.pixel_store.pack.skip_pixels); break;
    case GL_PACK_ALIGNMENT: i(s.pixel_store.pack.alignment); break;

    // Table 6.16 Pixels (cont.)
    case GL_MAP_COLOR: b(s.pixel_mode.map_color); break;
    case GL_MAP_STENCIL: b(s.pixel_mode.map_stencil); break;
    case GL_INDEX_SHIFT: i(s.pixel_mode.index_shift); break;
    case GL_INDEX_OFFSET: i(s.pixel_mode.index_offset); break;
    case GL_RED_SCALE: f(s.pixel_mode.scale[0]); break;
    case GL_GREEN_SCALE: f(s.pixel_mode.scale[1]); break;
    case GL_BLUE_SCALE: f(s.pixel_mode.scale[2]); break;
    case GL_ALPHA_SCALE: f(s.pixel_mode.scale[3]); break;
    case GL_DEPTH_SCALE: f(s.pixel_mode.depth_scale); break;
    case GL_RED_BIAS: f(s.pixel_mode.bias[0]); break;
    case GL_GREEN_BIAS: f(s.pixel_mode.bias[1]); break;
    case GL_BLUE_BIAS: f(s.pixel_mode.bias[2]); break;
    case GL_ALPHA_BIAS: f(s.pixel_mode.bias[3]); break;
    case GL_DEPTH_BIAS: f(s.pixel_mode.depth_bias); break;
    case GL_ZOOM_X: f(s.pixel_mode.zoom_x); break;
    case GL_ZOOM_Y: f(s.pixel_mode.zoom_y); break;
    case GL_READ_BUFFER: i(s.pixel_mode.read_buffer); break;
    case GL_PIXEL_MAP_I_TO_I_SIZE: case GL_PIXEL_MAP_S_TO_S_SIZE: case GL_PIXEL_MAP_I_TO_R_SIZE:
    case GL_PIXEL_MAP_I_TO_G_SIZE: case GL_PIXEL_MAP_I_TO_B_SIZE: case GL_PIXEL_MAP_I_TO_A_SIZE:
    case GL_PIXEL_MAP_R_TO_R_SIZE: case GL_PIXEL_MAP_G_TO_G_SIZE: case GL_PIXEL_MAP_B_TO_B_SIZE:
    case GL_PIXEL_MAP_A_TO_A_SIZE:
        i(static_cast<double>(ctx.pixel_maps[pname - GL_PIXEL_MAP_I_TO_I_SIZE].size()));
        break;

    // Table 6.17 Evaluators
    case GL_MAP1_GRID_DOMAIN:
        out.set(K::Float, {s.eval.map1_grid_domain[0], s.eval.map1_grid_domain[1]});
        break;
    case GL_MAP2_GRID_DOMAIN:
        out.set(K::Float, {s.eval.map2_grid_domain[0], s.eval.map2_grid_domain[1], s.eval.map2_grid_domain[2],
                           s.eval.map2_grid_domain[3]});
        break;
    case GL_MAP1_GRID_SEGMENTS: i(s.eval.map1_grid_segments); break;
    case GL_MAP2_GRID_SEGMENTS:
        out.set(K::Integer, {static_cast<double>(s.eval.map2_grid_segments[0]),
                             static_cast<double>(s.eval.map2_grid_segments[1])});
        break;

    // Table 6.18 Hints
    case GL_PERSPECTIVE_CORRECTION_HINT: i(s.hint.perspective_correction); break;
    case GL_POINT_SMOOTH_HINT: i(s.hint.point_smooth); break;
    case GL_LINE_SMOOTH_HINT: i(s.hint.line_smooth); break;
    case GL_POLYGON_SMOOTH_HINT: i(s.hint.polygon_smooth); break;
    case GL_FOG_HINT: i(s.hint.fog); break;

    // Tables 6.19-6.20 Implementation Dependent Values
    case GL_MAX_LIGHTS: i(kMaxLights); break;
    case GL_MAX_CLIP_PLANES: i(kMaxClipPlanes); break;
    case GL_MAX_MODELVIEW_STACK_DEPTH: i(kMaxModelviewStackDepth); break;
    case GL_MAX_PROJECTION_STACK_DEPTH: i(kMaxProjectionStackDepth); break;
    case GL_MAX_TEXTURE_STACK_DEPTH: i(kMaxTextureStackDepth); break;
    case GL_SUBPIXEL_BITS: i(kSubpixelBits); break;
    case GL_MAX_TEXTURE_SIZE: i(kMaxTextureSize); break;
    case GL_MAX_PIXEL_MAP_TABLE: i(kMaxPixelMapTable); break;
    case GL_MAX_NAME_STACK_DEPTH: i(kMaxNameStackDepth); break;
    case GL_MAX_LIST_NESTING: i(kMaxListNesting); break;
    case GL_MAX_EVAL_ORDER: i(kMaxEvalOrder); break;
    case GL_MAX_VIEWPORT_DIMS: out.set(K::Integer, {kMaxViewportDim, kMaxViewportDim}); break;
    case GL_MAX_ATTRIB_STACK_DEPTH: i(kMaxAttribStackDepth); break;
    case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH: i(kMaxClientAttribStackDepth); break;
    case GL_AUX_BUFFERS: i(fb.aux_buffers); break;
    case GL_RGBA_MODE: b(true); break;     // Vitreo formats are RGBA only (Design 8)
    case GL_INDEX_MODE: b(false); break;
    case GL_DOUBLEBUFFER: b(fb.double_buffer); break;
    case GL_STEREO: b(fb.stereo); break;
    case GL_POINT_SIZE_RANGE: out.set(K::Float, {kPointSizeRange[0], kPointSizeRange[1]}); break;
    case GL_POINT_SIZE_GRANULARITY: f(kPointSizeGranularity); break;
    case GL_LINE_WIDTH_RANGE: out.set(K::Float, {kLineWidthRange[0], kLineWidthRange[1]}); break;
    case GL_LINE_WIDTH_GRANULARITY: f(kLineWidthGranularity); break;

    // Table 6.21 Implementation Dependent Pixel Depths
    case GL_RED_BITS: i(fb.red_bits); break;
    case GL_GREEN_BITS: i(fb.green_bits); break;
    case GL_BLUE_BITS: i(fb.blue_bits); break;
    case GL_ALPHA_BITS: i(fb.alpha_bits); break;
    case GL_INDEX_BITS: i(fb.index_bits); break;
    case GL_DEPTH_BITS: i(fb.depth_bits); break;
    case GL_STENCIL_BITS: i(fb.stencil_bits); break;
    case GL_ACCUM_RED_BITS: i(fb.accum_red_bits); break;
    case GL_ACCUM_GREEN_BITS: i(fb.accum_green_bits); break;
    case GL_ACCUM_BLUE_BITS: i(fb.accum_blue_bits); break;
    case GL_ACCUM_ALPHA_BITS: i(fb.accum_alpha_bits); break;

    // Table 6.22 Miscellaneous
    case GL_LIST_BASE: i(s.list.base); break;
    case GL_LIST_INDEX: i(ctx.list_index); break;
    case GL_LIST_MODE: i(ctx.list_mode); break;
    case GL_ATTRIB_STACK_DEPTH: i(static_cast<double>(ctx.attrib_stack.size())); break;
    case GL_CLIENT_ATTRIB_STACK_DEPTH: i(static_cast<double>(ctx.client_attrib_stack.size())); break;
    case GL_NAME_STACK_DEPTH: i(static_cast<double>(ctx.name_stack.size())); break;
    case GL_RENDER_MODE: i(ctx.render_mode); break;
    case GL_SELECTION_BUFFER_SIZE: i(ctx.selection_buffer_size); break;
    case GL_FEEDBACK_BUFFER_SIZE: i(ctx.feedback_buffer_size); break;
    case GL_FEEDBACK_BUFFER_TYPE: i(ctx.feedback_buffer_type); break;

    default:
        ctx.record_error(GL_INVALID_ENUM);   // get-invalid-*: INVALID_ENUM, nothing written
        return false;
    }
    return true;
}

bool GetPointer(GlContext& ctx, u32 pname, GuestAddr& out) {
    if (ctx.reject_inside_begin_end()) return false;
    const GlVertexArrayState& va = ctx.state.vertex_array;
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: out = va.vertex.pointer; return true;
    case GL_NORMAL_ARRAY_POINTER: out = va.normal.pointer; return true;
    case GL_COLOR_ARRAY_POINTER: out = va.color.pointer; return true;
    case GL_INDEX_ARRAY_POINTER: out = va.index.pointer; return true;
    case GL_TEXTURE_COORD_ARRAY_POINTER: out = va.tex_coord.pointer; return true;
    case GL_EDGE_FLAG_ARRAY_POINTER: out = va.edge_flag.pointer; return true;
    case GL_SELECTION_BUFFER_POINTER: out = ctx.selection_buffer; return true;
    case GL_FEEDBACK_BUFFER_POINTER: out = ctx.feedback_buffer; return true;
    default:
        ctx.record_error(GL_INVALID_ENUM);   // getpointer-invalid-0x1234: untouched
        return false;
    }
}

}  // namespace cmd

}  // namespace vitreo::winapi::gl
