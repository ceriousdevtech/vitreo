// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_state_defaults.cpp
/// @brief Initial value of every GL 1.1 state-table item: one assignment per
///        row of the OpenGL 1.1 specification's Tables 6.5-6.22, in table
///        order, plus the enable -> attribute-group map of those tables.
///        Values the spec leaves to the implementation or to the window are
///        noted with the gl11_state_probe64.expected tag that measured them.

#include "gl_context.hpp"

namespace vitreo::winapi::gl {

void gl_set_initial_state(GlState& s, const GlFramebufferConfig& fb) {
    s = GlState{};

    // Table 6.5 Current Values and Associated Data (attribute: current).
    s.current.color = {1, 1, 1, 1};                 // CURRENT_COLOR
    s.current.index = 1;                            // CURRENT_INDEX
    s.current.tex_coords = {0, 0, 0, 1};            // CURRENT_TEXTURE_COORDS
    s.current.normal = {0, 0, 1};                   // CURRENT_NORMAL
    s.current.raster_position = {0, 0, 0, 1};       // CURRENT_RASTER_POSITION
    s.current.raster_distance = 0;                  // CURRENT_RASTER_DISTANCE
    s.current.raster_color = {1, 1, 1, 1};          // CURRENT_RASTER_COLOR
    s.current.raster_index = 1;                     // CURRENT_RASTER_INDEX
    s.current.raster_tex_coords = {0, 0, 0, 1};     // CURRENT_RASTER_TEXTURE_COORDS
    s.current.raster_position_valid = true;         // CURRENT_RASTER_POSITION_VALID
    s.current.edge_flag = true;                     // EDGE_FLAG

    // Table 6.6 Vertex Array Data (attribute: vertex-array). All disabled.
    auto array = [](i32 size) { return GlClientArray{false, size, GL_FLOAT, 0, 0}; };
    s.vertex_array.vertex = array(4);               // VERTEX_ARRAY, _SIZE 4, _TYPE FLOAT, _STRIDE 0, _POINTER 0
    s.vertex_array.normal = array(3);               // NORMAL_ARRAY, _TYPE FLOAT, _STRIDE 0, _POINTER 0
    s.vertex_array.color = array(4);                // COLOR_ARRAY, _SIZE 4, _TYPE FLOAT, _STRIDE 0, _POINTER 0
    s.vertex_array.index = array(1);                // INDEX_ARRAY, _TYPE FLOAT, _STRIDE 0, _POINTER 0
    s.vertex_array.tex_coord = array(4);            // TEXTURE_COORD_ARRAY, _SIZE 4, _TYPE FLOAT, ...
    s.vertex_array.edge_flag = array(1);            // EDGE_FLAG_ARRAY, _STRIDE 0, _POINTER 0 (GLboolean elements)

    // Table 6.7 Transformation state. The matrix stacks are GlContext state.
    s.viewport.x = 0;                               // VIEWPORT: set at first make-current (2.10.1)
    s.viewport.y = 0;
    s.viewport.width = 0;
    s.viewport.height = 0;
    s.viewport.depth_near = 0;                      // DEPTH_RANGE 0,1
    s.viewport.depth_far = 1;
    s.transform.matrix_mode = GL_MODELVIEW;         // MATRIX_MODE
    s.enable.set(GlCap::Normalize, false);          // NORMALIZE
    for (auto& plane : s.transform.clip_planes) plane = {0, 0, 0, 0};  // CLIP_PLANEi coefficients
    for (u32 i = 0; i < kMaxClipPlanes; ++i)                            // CLIP_PLANEi enables
        s.enable.set(gl_cap_offset(GlCap::ClipPlane0, i), false);

    // Table 6.8 Coloring.
    s.fog.color = {0, 0, 0, 0};                     // FOG_COLOR
    s.fog.index = 0;                                // FOG_INDEX
    s.fog.density = 1;                              // FOG_DENSITY
    s.fog.start = 0;                                // FOG_START
    s.fog.end = 1;                                  // FOG_END
    s.fog.mode = GL_EXP;                            // FOG_MODE
    s.enable.set(GlCap::Fog, false);                // FOG
    s.lighting.shade_model = GL_SMOOTH;             // SHADE_MODEL

    // Table 6.9 Lighting (see also Table 2.7 for light defaults).
    s.enable.set(GlCap::Lighting, false);           // LIGHTING
    s.enable.set(GlCap::ColorMaterial, false);      // COLOR_MATERIAL
    s.lighting.color_material_parameter = GL_AMBIENT_AND_DIFFUSE;  // COLOR_MATERIAL_PARAMETER
    s.lighting.color_material_face = GL_FRONT_AND_BACK;           // COLOR_MATERIAL_FACE
    for (GlMaterial& m : s.lighting.material) {
        m.ambient = {0.2f, 0.2f, 0.2f, 1.0f};       // AMBIENT (material)
        m.diffuse = {0.8f, 0.8f, 0.8f, 1.0f};       // DIFFUSE (material)
        m.specular = {0, 0, 0, 1};                  // SPECULAR (material)
        m.emission = {0, 0, 0, 1};                  // EMISSION
        m.shininess = 0;                            // SHININESS
        m.color_indexes = {0, 1, 1};                // COLOR_INDEXES
    }
    s.lighting.model_ambient = {0.2f, 0.2f, 0.2f, 1.0f};  // LIGHT_MODEL_AMBIENT
    s.lighting.model_local_viewer = false;          // LIGHT_MODEL_LOCAL_VIEWER
    s.lighting.model_two_side = false;              // LIGHT_MODEL_TWO_SIDE
    for (u32 i = 0; i < kMaxLights; ++i) {
        GlLight& l = s.lighting.lights[i];
        l.ambient = {0, 0, 0, 1};                   // AMBIENT (light)
        // DIFFUSE / SPECULAR (light): "see 2.5" = Table 2.7, (1,1,1,1) for
        // LIGHT0 and (0,0,0,1) for the others.
        l.diffuse = i == 0 ? GlVec4{1, 1, 1, 1} : GlVec4{0, 0, 0, 1};
        l.specular = i == 0 ? GlVec4{1, 1, 1, 1} : GlVec4{0, 0, 0, 1};
        l.position = {0, 0, 1, 0};                  // POSITION
        l.constant_attenuation = 1;                 // CONSTANT_ATTENUATION
        l.linear_attenuation = 0;                   // LINEAR_ATTENUATION
        l.quadratic_attenuation = 0;                // QUADRATIC_ATTENUATION
        l.spot_direction = {0, 0, -1};              // SPOT_DIRECTION
        l.spot_exponent = 0;                        // SPOT_EXPONENT
        l.spot_cutoff = 180;                        // SPOT_CUTOFF
        s.enable.set(gl_cap_offset(GlCap::Light0, i), false);  // LIGHTi
    }

    // Table 6.10 Rasterization.
    s.point.size = 1;                               // POINT_SIZE
    s.enable.set(GlCap::PointSmooth, false);        // POINT_SMOOTH
    s.line.width = 1;                               // LINE_WIDTH
    s.enable.set(GlCap::LineSmooth, false);         // LINE_SMOOTH
    s.line.stipple_pattern = 0xFFFF;                // LINE_STIPPLE_PATTERN 1's
    s.line.stipple_repeat = 1;                      // LINE_STIPPLE_REPEAT
    s.enable.set(GlCap::LineStipple, false);        // LINE_STIPPLE
    s.enable.set(GlCap::CullFace, false);           // CULL_FACE
    s.polygon.cull_face_mode = GL_BACK;             // CULL_FACE_MODE
    s.polygon.front_face = GL_CCW;                  // FRONT_FACE
    s.enable.set(GlCap::PolygonSmooth, false);      // POLYGON_SMOOTH
    s.polygon.mode = {GL_FILL, GL_FILL};            // POLYGON_MODE
    s.polygon.offset_factor = 0;                    // POLYGON_OFFSET_FACTOR
    s.polygon.offset_units = 0;                     // POLYGON_OFFSET_UNITS
    s.enable.set(GlCap::PolygonOffsetPoint, false); // POLYGON_OFFSET_POINT
    s.enable.set(GlCap::PolygonOffsetLine, false);  // POLYGON_OFFSET_LINE
    s.enable.set(GlCap::PolygonOffsetFill, false);  // POLYGON_OFFSET_FILL
    s.polygon_stipple.rows.fill(0xFFFFFFFFu);       // polygon stipple 1's
    s.enable.set(GlCap::PolygonStipple, false);     // POLYGON_STIPPLE

    // Table 6.11 Texture Objects: per-object state is gl_initial_texture_object.
    s.enable.set(GlCap::Texture1D, false);          // TEXTURE_1D
    s.enable.set(GlCap::Texture2D, false);          // TEXTURE_2D
    s.texture.binding_1d = 0;                       // TEXTURE_BINDING_1D
    s.texture.binding_2d = 0;                       // TEXTURE_BINDING_2D

    // Table 6.12 Texture Environment and Generation.
    s.texture.env_mode = GL_MODULATE;               // TEXTURE_ENV_MODE
    s.texture.env_color = {0, 0, 0, 0};             // TEXTURE_ENV_COLOR
    for (u32 i = 0; i < 4; ++i) {
        s.enable.set(gl_cap_offset(GlCap::TextureGenS, i), false);  // TEXTURE_GEN_x
        GlTexGen& g = s.texture.gen[i];
        // EYE_PLANE / OBJECT_PLANE "see 2.10.4": (1,0,0,0) for s, (0,1,0,0)
        // for t, 0 for r and q.
        g.object_plane = {0, 0, 0, 0};
        if (i < 2) g.object_plane[i] = 1;
        g.eye_plane = g.object_plane;
        g.mode = GL_EYE_LINEAR;                     // TEXTURE_GEN_MODE
    }

    // Table 6.13 Pixel Operations.
    s.enable.set(GlCap::ScissorTest, false);        // SCISSOR_TEST
    s.scissor = GlScissorState{0, 0, 0, 0};         // SCISSOR_BOX: set at first make-current (4.1.2)
    s.enable.set(GlCap::AlphaTest, false);          // ALPHA_TEST
    s.color_buffer.alpha_func = GL_ALWAYS;          // ALPHA_TEST_FUNC
    s.color_buffer.alpha_ref = 0;                   // ALPHA_TEST_REF
    s.enable.set(GlCap::StencilTest, false);        // STENCIL_TEST
    s.stencil_buffer.func = GL_ALWAYS;              // STENCIL_FUNC
    // STENCIL_VALUE_MASK 1's: the stencil buffer's s bits
    // (get-GL_STENCIL_VALUE_MASK 255 with 8 stencil bits).
    const u32 stencil_ones = fb.stencil_bits >= 32 ? 0xFFFFFFFFu : (1u << fb.stencil_bits) - 1u;
    s.stencil_buffer.value_mask = stencil_ones;
    s.stencil_buffer.ref = 0;                       // STENCIL_REF
    s.stencil_buffer.fail = GL_KEEP;                // STENCIL_FAIL
    s.stencil_buffer.pass_depth_fail = GL_KEEP;     // STENCIL_PASS_DEPTH_FAIL
    s.stencil_buffer.pass_depth_pass = GL_KEEP;     // STENCIL_PASS_DEPTH_PASS
    s.enable.set(GlCap::DepthTest, false);          // DEPTH_TEST
    s.depth_buffer.func = GL_LESS;                  // DEPTH_FUNC
    s.enable.set(GlCap::Blend, false);              // BLEND
    s.color_buffer.blend_src = GL_ONE;              // BLEND_SRC
    s.color_buffer.blend_dst = GL_ZERO;             // BLEND_DST
    s.enable.set(GlCap::Dither, true);              // DITHER
    s.enable.set(GlCap::IndexLogicOp, false);       // INDEX_LOGIC_OP
    s.enable.set(GlCap::ColorLogicOp, false);       // COLOR_LOGIC_OP
    s.color_buffer.logic_op_mode = GL_COPY;         // LOGIC_OP_MODE

    // Table 6.14 Framebuffer Control.
    // DRAW_BUFFER "see 4.2.1": BACK with a back buffer, FRONT otherwise.
    s.color_buffer.draw_buffer = fb.double_buffer ? GL_BACK : GL_FRONT;
    // INDEX_WRITEMASK: an RGBA context reports 0 (get-GL_INDEX_WRITEMASK).
    s.color_buffer.index_writemask = 0;
    s.color_buffer.color_writemask = {true, true, true, true};  // COLOR_WRITEMASK
    s.depth_buffer.writemask = true;                // DEPTH_WRITEMASK
    s.stencil_buffer.writemask = stencil_ones;      // STENCIL_WRITEMASK 1's (get-GL_STENCIL_WRITEMASK 255)
    s.color_buffer.clear_color = {0, 0, 0, 0};      // COLOR_CLEAR_VALUE
    s.color_buffer.clear_index = 0;                 // INDEX_CLEAR_VALUE
    s.depth_buffer.clear_value = 1;                 // DEPTH_CLEAR_VALUE
    s.stencil_buffer.clear_value = 0;               // STENCIL_CLEAR_VALUE
    s.accum_buffer.clear_value = {0, 0, 0, 0};      // ACCUM_CLEAR_VALUE

    // Table 6.15 Pixels (attribute: pixel-store).
    for (GlPixelStoreModes* m : {&s.pixel_store.unpack, &s.pixel_store.pack}) {
        m->swap_bytes = false;                      // x_SWAP_BYTES
        m->lsb_first = false;                       // x_LSB_FIRST
        m->row_length = 0;                          // x_ROW_LENGTH
        m->skip_rows = 0;                           // x_SKIP_ROWS
        m->skip_pixels = 0;                         // x_SKIP_PIXELS
        m->alignment = 4;                           // x_ALIGNMENT
    }

    // Table 6.16 Pixels (cont.) (attribute: pixel). Maps are GlContext state.
    s.pixel_mode.map_color = false;                 // MAP_COLOR
    s.pixel_mode.map_stencil = false;               // MAP_STENCIL
    s.pixel_mode.index_shift = 0;                   // INDEX_SHIFT
    s.pixel_mode.index_offset = 0;                  // INDEX_OFFSET
    s.pixel_mode.scale = {1, 1, 1, 1};              // x_SCALE
    s.pixel_mode.depth_scale = 1;                   // DEPTH_SCALE
    s.pixel_mode.bias = {0, 0, 0, 0};               // x_BIAS
    s.pixel_mode.depth_bias = 0;                    // DEPTH_BIAS
    s.pixel_mode.zoom_x = 1;                        // ZOOM_X
    s.pixel_mode.zoom_y = 1;                        // ZOOM_Y
    // READ_BUFFER "see 4.3.2": BACK with a back buffer, FRONT otherwise.
    s.pixel_mode.read_buffer = fb.double_buffer ? GL_BACK : GL_FRONT;

    // Table 6.17 Evaluators. Maps themselves are GlContext state.
    for (u32 i = 0; i < 9; ++i) {
        s.enable.set(gl_cap_offset(GlCap::Map1Color4, i), false);  // MAP1_x
        s.enable.set(gl_cap_offset(GlCap::Map2Color4, i), false);  // MAP2_x
    }
    s.eval.map1_grid_domain = {0, 1};               // MAP1_GRID_DOMAIN
    s.eval.map2_grid_domain = {0, 1, 0, 1};         // MAP2_GRID_DOMAIN
    s.eval.map1_grid_segments = 1;                  // MAP1_GRID_SEGMENTS
    s.eval.map2_grid_segments = {1, 1};             // MAP2_GRID_SEGMENTS
    s.enable.set(GlCap::AutoNormal, false);         // AUTO_NORMAL

    // Table 6.18 Hints.
    s.hint.perspective_correction = GL_DONT_CARE;   // PERSPECTIVE_CORRECTION_HINT
    s.hint.point_smooth = GL_DONT_CARE;             // POINT_SMOOTH_HINT
    s.hint.line_smooth = GL_DONT_CARE;              // LINE_SMOOTH_HINT
    s.hint.polygon_smooth = GL_DONT_CARE;           // POLYGON_SMOOTH_HINT
    s.hint.fog = GL_DONT_CARE;                      // FOG_HINT

    // Tables 6.19-6.21 are constants (gl_context.hpp) and the framebuffer config.

    // Table 6.22 Miscellaneous. LIST_INDEX/LIST_MODE, the attribute stacks,
    // NAME_STACK_DEPTH, RENDER_MODE and the selection/feedback buffers are
    // GlContext state.
    s.list.base = 0;                                // LIST_BASE
}

GlTextureObject gl_initial_texture_object(u32 name) {
    // Table 6.11 Texture Objects.
    GlTextureObject t;
    t.name = name;
    t.params.border_color = {0, 0, 0, 0};           // TEXTURE_BORDER_COLOR
    t.params.min_filter = GL_NEAREST_MIPMAP_LINEAR; // TEXTURE_MIN_FILTER "see 3.8"
    t.params.mag_filter = GL_LINEAR;                // TEXTURE_MAG_FILTER "see 3.8"
    t.params.wrap_s = GL_REPEAT;                    // TEXTURE_WRAP_S
    t.params.wrap_t = GL_REPEAT;                    // TEXTURE_WRAP_T
    t.params.priority = 1;                          // TEXTURE_PRIORITY
    t.resident = false;                             // TEXTURE_RESIDENT
    for (GlTexLevel& level : t.levels) {
        level = GlTexLevel{};
        level.width = 0;                            // TEXTURE_WIDTH
        level.height = 0;                           // TEXTURE_HEIGHT
        level.border = 0;                           // TEXTURE_BORDER
        level.internal_format = 1;                  // TEXTURE_INTERNAL_FORMAT (COMPONENTS) 1
        // TEXTURE_{RED,GREEN,BLUE,ALPHA,LUMINANCE,INTENSITY}_SIZE 0
    }
    return t;
}

void gl_set_initial_eval_maps(std::array<GlEvalMap, 9>& map1, std::array<GlEvalMap, 9>& map2) {
    // Spec 5.1 / Table 5.1: every map has order 1, domain [0,1] (x [0,1]) and
    // the single control point below.
    static const std::array<std::vector<float>, 9> kInitialPoint = {{
        {1, 1, 1, 1},   // MAPx_COLOR_4
        {1},            // MAPx_INDEX
        {0, 0, 1},      // MAPx_NORMAL
        {0},            // MAPx_TEXTURE_COORD_1
        {0, 0},         // MAPx_TEXTURE_COORD_2
        {0, 0, 0},      // MAPx_TEXTURE_COORD_3
        {0, 0, 0, 1},   // MAPx_TEXTURE_COORD_4
        {0, 0, 0},      // MAPx_VERTEX_3
        {0, 0, 0, 1},   // MAPx_VERTEX_4
    }};
    for (u32 i = 0; i < 9; ++i) {
        map1[i] = GlEvalMap{1, 1, 0, 1, 0, 1, kInitialPoint[i]};
        map2[i] = GlEvalMap{1, 1, 0, 1, 0, 1, kInitialPoint[i]};
    }
}

void gl_set_initial_pixel_maps(std::array<std::vector<float>, 10>& maps) {
    // Table 3.3: every map has size 1 and the single entry 0.
    for (auto& map : maps) map.assign(1, 0.0f);
}

// ---- Enables: GL enum and attribute group of each capability ----------------

namespace {
struct CapInfo {
    u32 gl_enum;
    u32 group;   // attribute group besides ENABLE_BIT ("x/enable" in Tables 6.7-6.17)
};

// Indexed by GlCap.
constexpr CapInfo kCaps[kGlCapCount] = {
    {GL_ALPHA_TEST, GL_COLOR_BUFFER_BIT},           // Table 6.13 color-buffer/enable
    {GL_AUTO_NORMAL, GL_EVAL_BIT},                  // Table 6.17 eval/enable
    {GL_BLEND, GL_COLOR_BUFFER_BIT},                // Table 6.13
    {GL_CLIP_PLANE0, GL_TRANSFORM_BIT},             // Table 6.7 transform/enable
    {GL_CLIP_PLANE1, GL_TRANSFORM_BIT},
    {GL_CLIP_PLANE2, GL_TRANSFORM_BIT},
    {GL_CLIP_PLANE3, GL_TRANSFORM_BIT},
    {GL_CLIP_PLANE4, GL_TRANSFORM_BIT},
    {GL_CLIP_PLANE5, GL_TRANSFORM_BIT},
    {GL_COLOR_LOGIC_OP, GL_COLOR_BUFFER_BIT},       // Table 6.13
    {GL_COLOR_MATERIAL, GL_LIGHTING_BIT},           // Table 6.9 lighting/enable
    {GL_CULL_FACE, GL_POLYGON_BIT},                 // Table 6.10 polygon/enable
    {GL_DEPTH_TEST, GL_DEPTH_BUFFER_BIT},           // Table 6.13 depth-buffer/enable
    {GL_DITHER, GL_COLOR_BUFFER_BIT},               // Table 6.13
    {GL_FOG, GL_FOG_BIT},                           // Table 6.8 fog/enable
    {GL_INDEX_LOGIC_OP, GL_COLOR_BUFFER_BIT},       // Table 6.13
    {GL_LIGHT0, GL_LIGHTING_BIT},                   // Table 6.9
    {GL_LIGHT1, GL_LIGHTING_BIT},
    {GL_LIGHT2, GL_LIGHTING_BIT},
    {GL_LIGHT3, GL_LIGHTING_BIT},
    {GL_LIGHT4, GL_LIGHTING_BIT},
    {GL_LIGHT5, GL_LIGHTING_BIT},
    {GL_LIGHT6, GL_LIGHTING_BIT},
    {GL_LIGHT7, GL_LIGHTING_BIT},
    {GL_LIGHTING, GL_LIGHTING_BIT},                 // Table 6.9
    {GL_LINE_SMOOTH, GL_LINE_BIT},                  // Table 6.10 line/enable
    {GL_LINE_STIPPLE, GL_LINE_BIT},
    {GL_MAP1_COLOR_4, GL_EVAL_BIT},                 // Table 6.17
    {GL_MAP1_INDEX, GL_EVAL_BIT},
    {GL_MAP1_NORMAL, GL_EVAL_BIT},
    {GL_MAP1_TEXTURE_COORD_1, GL_EVAL_BIT},
    {GL_MAP1_TEXTURE_COORD_2, GL_EVAL_BIT},
    {GL_MAP1_TEXTURE_COORD_3, GL_EVAL_BIT},
    {GL_MAP1_TEXTURE_COORD_4, GL_EVAL_BIT},
    {GL_MAP1_VERTEX_3, GL_EVAL_BIT},
    {GL_MAP1_VERTEX_4, GL_EVAL_BIT},
    {GL_MAP2_COLOR_4, GL_EVAL_BIT},
    {GL_MAP2_INDEX, GL_EVAL_BIT},
    {GL_MAP2_NORMAL, GL_EVAL_BIT},
    {GL_MAP2_TEXTURE_COORD_1, GL_EVAL_BIT},
    {GL_MAP2_TEXTURE_COORD_2, GL_EVAL_BIT},
    {GL_MAP2_TEXTURE_COORD_3, GL_EVAL_BIT},
    {GL_MAP2_TEXTURE_COORD_4, GL_EVAL_BIT},
    {GL_MAP2_VERTEX_3, GL_EVAL_BIT},
    {GL_MAP2_VERTEX_4, GL_EVAL_BIT},
    {GL_NORMALIZE, GL_TRANSFORM_BIT},               // Table 6.7 transform/enable
    {GL_POINT_SMOOTH, GL_POINT_BIT},                // Table 6.10 point/enable
    {GL_POLYGON_OFFSET_FILL, GL_POLYGON_BIT},       // Table 6.10
    {GL_POLYGON_OFFSET_LINE, GL_POLYGON_BIT},
    {GL_POLYGON_OFFSET_POINT, GL_POLYGON_BIT},
    {GL_POLYGON_SMOOTH, GL_POLYGON_BIT},
    {GL_POLYGON_STIPPLE, GL_POLYGON_BIT},           // Table 6.10 polygon/enable
    {GL_SCISSOR_TEST, GL_SCISSOR_BIT},              // Table 6.13 scissor/enable
    {GL_STENCIL_TEST, GL_STENCIL_BUFFER_BIT},       // Table 6.13 stencil-buffer/enable
    {GL_TEXTURE_1D, GL_TEXTURE_BIT},                // Table 6.11 texture/enable
    {GL_TEXTURE_2D, GL_TEXTURE_BIT},
    {GL_TEXTURE_GEN_S, GL_TEXTURE_BIT},             // Table 6.12 texture/enable
    {GL_TEXTURE_GEN_T, GL_TEXTURE_BIT},
    {GL_TEXTURE_GEN_R, GL_TEXTURE_BIT},
    {GL_TEXTURE_GEN_Q, GL_TEXTURE_BIT},
};
}  // namespace

std::optional<GlCap> gl_cap_from_enum(u32 cap) noexcept {
    for (u32 i = 0; i < kGlCapCount; ++i) {
        if (kCaps[i].gl_enum == cap) return static_cast<GlCap>(i);
    }
    return std::nullopt;
}

u32 gl_cap_enum(GlCap cap) noexcept { return kCaps[static_cast<u32>(cap)].gl_enum; }

u32 gl_cap_group(GlCap cap) noexcept { return kCaps[static_cast<u32>(cap)].group; }

}  // namespace vitreo::winapi::gl
