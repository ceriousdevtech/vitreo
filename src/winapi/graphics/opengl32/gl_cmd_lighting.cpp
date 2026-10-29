// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_lighting.cpp
/// @brief Lighting, material, colour-material, shade-model and fog state
///        (OpenGL 1.1 spec 2.13 and 3.9).

#include "gl_cmd_internal.hpp"

namespace vitreo::winapi::gl {

namespace detail {

void apply_color_material(GlContext& ctx) noexcept {
    GlState& s = ctx.state;
    if (!s.enabled(GlCap::ColorMaterial)) return;
    const GlVec4& c = s.current.color;
    const u32 face = s.lighting.color_material_face;
    for (u32 side = 0; side < 2; ++side) {
        if (side == 0 && face == GL_BACK) continue;
        if (side == 1 && face == GL_FRONT) continue;
        GlMaterial& m = s.lighting.material[side];
        switch (s.lighting.color_material_parameter) {
        case GL_EMISSION: m.emission = c; break;
        case GL_AMBIENT: m.ambient = c; break;
        case GL_DIFFUSE: m.diffuse = c; break;
        case GL_SPECULAR: m.specular = c; break;
        case GL_AMBIENT_AND_DIFFUSE: m.ambient = c; m.diffuse = c; break;
        default: break;
        }
    }
}

}  // namespace detail

namespace cmd {

using detail::color_component;

namespace {

bool is_light(u32 light) { return light >= GL_LIGHT0 && light < GL_LIGHT0 + kMaxLights; }

bool is_face(u32 face) { return face == GL_FRONT || face == GL_BACK || face == GL_FRONT_AND_BACK; }

GlVec4 color4(const GlValues& v) {
    return GlVec4{static_cast<float>(color_component(v, 0)), static_cast<float>(color_component(v, 1)),
                  static_cast<float>(color_component(v, 2)), static_cast<float>(color_component(v, 3))};
}

}  // namespace

void ShadeModel(GlContext& ctx, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;
    if (mode != GL_FLAT && mode != GL_SMOOTH) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    ctx.state.lighting.shade_model = mode;
}

void Light(GlContext& ctx, u32 light, u32 pname, const GlValues& values) {
    if (ctx.reject_inside_begin_end()) return;
    if (!is_light(light)) {
        ctx.record_error(GL_INVALID_ENUM);   // light8-GL_AMBIENT-out-of-range, err-bad-enum-glLightfv-light
        return;
    }
    GlLight& l = ctx.state.lighting.lights[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT:
    case GL_DIFFUSE:
    case GL_SPECULAR:
    case GL_POSITION:
    case GL_SPOT_DIRECTION:
        if (values.scalar) {
            ctx.record_error(GL_INVALID_ENUM);   // a vector parameter through glLight{f,i}
            return;
        }
        break;
    case GL_SPOT_EXPONENT:
    case GL_SPOT_CUTOFF:
    case GL_CONSTANT_ATTENUATION:
    case GL_LINEAR_ATTENUATION:
    case GL_QUADRATIC_ATTENUATION:
        break;
    default:
        ctx.record_error(GL_INVALID_ENUM);   // err-bad-enum-glLightfv-pname
        return;
    }
    const float v0 = static_cast<float>(values[0]);
    switch (pname) {
    case GL_AMBIENT: l.ambient = color4(values); break;
    case GL_DIFFUSE: l.diffuse = color4(values); break;
    case GL_SPECULAR: l.specular = color4(values); break;
    case GL_POSITION: {
        // Spec 2.13.2: transformed by the model-view matrix when specified.
        const auto eye = detail::transform_point(ctx.modelview(), {values[0], values[1], values[2], values[3]});
        l.position = {static_cast<float>(eye[0]), static_cast<float>(eye[1]),
                      static_cast<float>(eye[2]), static_cast<float>(eye[3])};
        break;
    }
    case GL_SPOT_DIRECTION: {
        // Spec 2.13.2: transformed by the upper-left 3x3 of the model-view matrix.
        const GlMatrix& m = ctx.modelview();
        for (u32 row = 0; row < 3; ++row) {
            l.spot_direction[row] = static_cast<float>(m[0 * 4 + row] * values[0] + m[1 * 4 + row] * values[1] +
                                                       m[2 * 4 + row] * values[2]);
        }
        break;
    }
    case GL_SPOT_EXPONENT:
        if (v0 < 0 || v0 > 128) { ctx.record_error(GL_INVALID_VALUE); return; }   // Table 2.7
        l.spot_exponent = v0;
        break;
    case GL_SPOT_CUTOFF:
        if ((v0 < 0 || v0 > 90) && v0 != 180) { ctx.record_error(GL_INVALID_VALUE); return; }
        l.spot_cutoff = v0;
        break;
    case GL_CONSTANT_ATTENUATION:
    case GL_LINEAR_ATTENUATION:
    case GL_QUADRATIC_ATTENUATION:
        if (v0 < 0) { ctx.record_error(GL_INVALID_VALUE); return; }
        (pname == GL_CONSTANT_ATTENUATION ? l.constant_attenuation
         : pname == GL_LINEAR_ATTENUATION ? l.linear_attenuation
                                          : l.quadratic_attenuation) = v0;
        break;
    default: break;
    }
}

void LightModel(GlContext& ctx, u32 pname, const GlValues& values) {
    if (ctx.reject_inside_begin_end()) return;
    GlLightingState& lt = ctx.state.lighting;
    switch (pname) {
    case GL_LIGHT_MODEL_AMBIENT:
        if (values.scalar) { ctx.record_error(GL_INVALID_ENUM); return; }
        lt.model_ambient = color4(values);
        break;
    case GL_LIGHT_MODEL_LOCAL_VIEWER: lt.model_local_viewer = values[0] != 0; break;
    case GL_LIGHT_MODEL_TWO_SIDE: lt.model_two_side = values[0] != 0; break;
    default: ctx.record_error(GL_INVALID_ENUM); break;
    }
}

void Material(GlContext& ctx, u32 face, u32 pname, const GlValues& values) {
    // Material is allowed between Begin and End (spec 2.6.3).
    if (!is_face(face)) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
    case GL_AMBIENT:
    case GL_DIFFUSE:
    case GL_SPECULAR:
    case GL_EMISSION:
    case GL_AMBIENT_AND_DIFFUSE:
    case GL_COLOR_INDEXES:
        if (values.scalar) { ctx.record_error(GL_INVALID_ENUM); return; }
        break;
    case GL_SHININESS:
        if (values[0] < 0 || values[0] > 128) {   // Table 2.7; err-bad-value-glMaterialf-shininess
            ctx.record_error(GL_INVALID_VALUE);
            return;
        }
        break;
    default: ctx.record_error(GL_INVALID_ENUM); return;
    }
    for (u32 side = 0; side < 2; ++side) {
        if ((side == 0 && face == GL_BACK) || (side == 1 && face == GL_FRONT)) continue;
        GlMaterial& m = ctx.state.lighting.material[side];
        switch (pname) {
        case GL_AMBIENT: m.ambient = color4(values); break;
        case GL_DIFFUSE: m.diffuse = color4(values); break;
        case GL_SPECULAR: m.specular = color4(values); break;
        case GL_EMISSION: m.emission = color4(values); break;
        case GL_AMBIENT_AND_DIFFUSE: m.ambient = m.diffuse = color4(values); break;
        case GL_SHININESS: m.shininess = static_cast<float>(values[0]); break;
        case GL_COLOR_INDEXES:
            m.color_indexes = {static_cast<float>(values[0]), static_cast<float>(values[1]),
                               static_cast<float>(values[2])};
            break;
        default: break;
        }
    }
}

void ColorMaterial(GlContext& ctx, u32 face, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;
    const bool mode_ok = mode == GL_EMISSION || mode == GL_AMBIENT || mode == GL_DIFFUSE ||
                         mode == GL_SPECULAR || mode == GL_AMBIENT_AND_DIFFUSE;
    if (!is_face(face) || !mode_ok) {
        ctx.record_error(GL_INVALID_ENUM);   // err-glColorMaterial-bad-enum
        return;
    }
    ctx.state.lighting.color_material_face = face;
    ctx.state.lighting.color_material_parameter = mode;
    // Spec 2.13.3: "calling ColorMaterial(FRONT, AMBIENT) while COLOR_MATERIAL
    // is enabled sets the front material a_cm to the value of the current color".
    detail::apply_color_material(ctx);
}

bool GetLight(GlContext& ctx, u32 light, u32 pname, GlQuery& out) {
    if (ctx.reject_inside_begin_end()) return false;
    if (!is_light(light)) {
        ctx.record_error(GL_INVALID_ENUM);
        return false;
    }
    const GlLight& l = ctx.state.lighting.lights[light - GL_LIGHT0];
    auto c4 = [&](const GlVec4& c) { out.set(GlQueryKind::Color, {c[0], c[1], c[2], c[3]}); };
    switch (pname) {
    case GL_AMBIENT: c4(l.ambient); break;
    case GL_DIFFUSE: c4(l.diffuse); break;
    case GL_SPECULAR: c4(l.specular); break;
    case GL_POSITION:
        out.set(GlQueryKind::Float, {l.position[0], l.position[1], l.position[2], l.position[3]});
        break;
    case GL_SPOT_DIRECTION:
        out.set(GlQueryKind::Float, {l.spot_direction[0], l.spot_direction[1], l.spot_direction[2]});
        break;
    case GL_SPOT_EXPONENT: out.set(GlQueryKind::Float, {l.spot_exponent}); break;
    case GL_SPOT_CUTOFF: out.set(GlQueryKind::Float, {l.spot_cutoff}); break;
    case GL_CONSTANT_ATTENUATION: out.set(GlQueryKind::Float, {l.constant_attenuation}); break;
    case GL_LINEAR_ATTENUATION: out.set(GlQueryKind::Float, {l.linear_attenuation}); break;
    case GL_QUADRATIC_ATTENUATION: out.set(GlQueryKind::Float, {l.quadratic_attenuation}); break;
    default: ctx.record_error(GL_INVALID_ENUM); return false;
    }
    return true;
}

bool GetMaterial(GlContext& ctx, u32 face, u32 pname, GlQuery& out) {
    if (ctx.reject_inside_begin_end()) return false;
    // Spec 6.1.3: face must be FRONT or BACK.
    if (face != GL_FRONT && face != GL_BACK) {
        ctx.record_error(GL_INVALID_ENUM);
        return false;
    }
    const GlMaterial& m = ctx.state.lighting.material[face == GL_FRONT ? 0 : 1];
    auto c4 = [&](const GlVec4& c) { out.set(GlQueryKind::Color, {c[0], c[1], c[2], c[3]}); };
    switch (pname) {
    case GL_AMBIENT: c4(m.ambient); break;
    case GL_DIFFUSE: c4(m.diffuse); break;
    case GL_SPECULAR: c4(m.specular); break;
    case GL_EMISSION: c4(m.emission); break;
    case GL_SHININESS: out.set(GlQueryKind::Float, {m.shininess}); break;
    case GL_COLOR_INDEXES:
        out.set(GlQueryKind::Float, {m.color_indexes[0], m.color_indexes[1], m.color_indexes[2]});
        break;
    default: ctx.record_error(GL_INVALID_ENUM); return false;
    }
    return true;
}

void Fog(GlContext& ctx, u32 pname, const GlValues& values) {
    if (ctx.reject_inside_begin_end()) return;
    GlFogState& f = ctx.state.fog;
    const double v0 = values[0];
    switch (pname) {
    case GL_FOG_MODE: {
        const auto mode = detail::as_enum(v0);
        if (!mode || (*mode != GL_EXP && *mode != GL_EXP2 && *mode != GL_LINEAR)) {
            ctx.record_error(GL_INVALID_ENUM);   // err-bad-param-glFogi-mode
            return;
        }
        f.mode = *mode;
        break;
    }
    case GL_FOG_DENSITY:
        if (v0 < 0) { ctx.record_error(GL_INVALID_VALUE); return; }   // err-bad-value-glFogf-neg-density
        f.density = static_cast<float>(v0);
        break;
    case GL_FOG_START: f.start = static_cast<float>(v0); break;
    case GL_FOG_END: f.end = static_cast<float>(v0); break;
    case GL_FOG_INDEX: f.index = static_cast<float>(v0); break;
    case GL_FOG_COLOR:
        if (values.scalar) { ctx.record_error(GL_INVALID_ENUM); return; }
        // Spec 3.9: Table 2.6 signed conversion for integers, then clamped to [0, 1].
        for (u32 i = 0; i < 4; ++i) f.color[i] = detail::clamp01(color_component(values, i));
        break;
    default: ctx.record_error(GL_INVALID_ENUM); break;   // err-bad-enum-glFogi
    }
}

}  // namespace cmd

}  // namespace vitreo::winapi::gl
