// SPDX-License-Identifier: LGPL-2.1-only

/// @file ff_uniforms.cpp
/// @brief Uniform-buffer packing for `generate_ff_shaders` output.

#include "ff_uniforms.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace vitreo::graphics {
namespace {

void append_bytes(std::vector<u8>& out, const void* data, usize length) {
    const auto* bytes = static_cast<const u8*>(data);
    out.insert(out.end(), bytes, bytes + length);
}

void append_matrix(std::vector<u8>& out, const FFMatrix& m) {
    append_bytes(out, m.data(), sizeof(m));
}

void append_float4(std::vector<u8>& out, float x, float y, float z, float w) {
    const std::array<float, 4> value{x, y, z, w};
    append_bytes(out, value.data(), sizeof(value));
}

/// The world matrix for blend index `i`, clamped to the palette the caller
/// supplies. Direct3D's palette is finite; an out-of-range index is not a
/// contract the guest can observe because the shader clamps as well.
[[nodiscard]] const FFMatrix& world_matrix(const FFUniformInputs& inputs,
                                           u32 index) {
    return inputs.world_matrices[std::min(index, FF_MAX_WORLD_MATRICES - 1)];
}

/// The normal matrix for `m` (the caller passes world * view): the raw
/// inverse M^-1. MSL reads the bytes column-major, so its value is
/// transpose(M^-1), and the generator's row-vector form
/// `n * cb.world_inv_transpose` is Direct3D's camera-space normal
/// N * (wvMatrix^-1)^T (Learn, "Camera Space Transformations (Direct3D 9)";
/// ff_emulation.hpp, matrix convention). Fall back to `transpose(m)`, whose
/// MSL value is M itself, when `m` is singular.
void append_normal_matrix(std::vector<u8>& out, const FFMatrix& m) {
    FFMatrix inverse{};
    if (ff_matrix_invert(m, inverse)) {
        append_matrix(out, inverse);
    } else {
        append_matrix(out, ff_matrix_transpose(m));
    }
}

} // namespace

FFMatrix ff_matrix_multiply(const FFMatrix& a, const FFMatrix& b) {
    FFMatrix result{};
    for (usize row = 0; row < 4; ++row) {
        for (usize column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (usize k = 0; k < 4; ++k) {
                sum += a[row * 4 + k] * b[k * 4 + column];
            }
            result[row * 4 + column] = sum;
        }
    }
    return result;
}

FFMatrix ff_matrix_transpose(const FFMatrix& m) {
    FFMatrix result{};
    for (usize row = 0; row < 4; ++row) {
        for (usize column = 0; column < 4; ++column) {
            result[column * 4 + row] = m[row * 4 + column];
        }
    }
    return result;
}

bool ff_matrix_invert(const FFMatrix& m, FFMatrix& out) {
    const auto at = [&](usize row, usize col) -> float {
        return m[row * 4 + col];
    };
    const float a00 = at(0, 0), a01 = at(0, 1), a02 = at(0, 2), a03 = at(0, 3);
    const float a10 = at(1, 0), a11 = at(1, 1), a12 = at(1, 2), a13 = at(1, 3);
    const float a20 = at(2, 0), a21 = at(2, 1), a22 = at(2, 2), a23 = at(2, 3);
    const float a30 = at(3, 0), a31 = at(3, 1), a32 = at(3, 2), a33 = at(3, 3);

    const float b00 = a00 * a11 - a01 * a10;
    const float b01 = a00 * a12 - a02 * a10;
    const float b02 = a00 * a13 - a03 * a10;
    const float b03 = a01 * a12 - a02 * a11;
    const float b04 = a01 * a13 - a03 * a11;
    const float b05 = a02 * a13 - a03 * a12;
    const float b06 = a20 * a31 - a21 * a30;
    const float b07 = a20 * a32 - a22 * a30;
    const float b08 = a20 * a33 - a23 * a30;
    const float b09 = a21 * a32 - a22 * a31;
    const float b10 = a21 * a33 - a23 * a31;
    const float b11 = a22 * a33 - a23 * a32;

    float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 +
                b05 * b06;
    if (det == 0.0f || !std::isfinite(det)) {
        return false;
    }
    det = 1.0f / det;

    out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det;
    out[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
    out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det;
    out[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
    out[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det;
    out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
    out[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det;
    out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
    out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det;
    out[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
    out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det;
    out[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
    out[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det;
    out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
    out[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det;
    out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;
    return true;
}

std::array<float, 4> ff_color_to_rgba(u32 color) {
    const auto channel = [](u32 value) {
        return static_cast<float>(value & 0xFF) / 255.0f;
    };
    return {channel(color >> 16), channel(color >> 8), channel(color),
            channel(color >> 24)};
}

void pack_ff_vertex_uniforms(const FFPipelineState& state,
                             const FFUniformInputs& inputs,
                             std::vector<u8>& out) {
    out.clear();

    const FFMatrix& world = inputs.world_matrices[0];
    const FFMatrix& view = inputs.view;
    const FFMatrix& projection = inputs.projection;
    const FFMatrix world_view_proj =
        ff_matrix_multiply(ff_matrix_multiply(world, view), projection);

    append_matrix(out, ff_matrix_transpose(world));
    append_matrix(out, ff_matrix_transpose(view));
    append_matrix(out, ff_matrix_transpose(projection));
    append_matrix(out, ff_matrix_transpose(world_view_proj));
    // Normals are transformed to camera space by the world-view inverse
    // transpose (Learn, "Camera Space Transformations (Direct3D 9)"). GL
    // passes view = I, so this is MV^-1 for it (spec 2.10.3).
    append_normal_matrix(out, ff_matrix_multiply(world, view));

    if (state.indexed_vertex_blend) {
        // Keep the full palette resident so packed indices can select any
        // contractually valid WORLD matrix at draw time.
        for (u32 i = 0; i < FF_MAX_WORLD_MATRICES; ++i) {
            append_matrix(out, ff_matrix_transpose(world_matrix(inputs, i)));
        }
        for (u32 i = 0; i < FF_MAX_WORLD_MATRICES; ++i) {
            append_normal_matrix(out, ff_matrix_multiply(world_matrix(inputs, i), view));
        }
    } else if (state.vertex_blend_weight_count != 0) {
        for (u32 i = 1; i <= state.vertex_blend_weight_count; ++i) {
            const FFMatrix& blend_world = world_matrix(inputs, i);
            append_matrix(out, ff_matrix_transpose(blend_world));
            append_normal_matrix(out, ff_matrix_multiply(blend_world, view));
        }
    }
    if (state.vertex_tweening) {
        append_float4(out, state.tween_factor, 0.0f, 0.0f, 0.0f);
    }

    if ((state.fvf_flags & fvf::POSITION_MASK) == fvf::XYZRHW) {
        append_float4(out, inputs.viewport_width, inputs.viewport_height, 0.0f,
                      0.0f);
    }

    if (state.point_size_output) {
        const float minimum = std::max(0.0f, inputs.point_size_min);
        const float maximum =
            std::min(256.0f, std::max(0.0f, inputs.point_size_max));
        const float size =
            state.point_scale_enabled
                ? inputs.point_size
                : std::max(minimum, std::min(inputs.point_size, maximum));
        append_float4(out, size, minimum, maximum, 0.0f);
        if (state.point_scale_enabled) {
            append_float4(out, std::max(0.0f, inputs.point_scale_a),
                          std::max(0.0f, inputs.point_scale_b),
                          std::max(0.0f, inputs.point_scale_c),
                          inputs.viewport_height);
        }
    }

    if (state.clip_planes_enabled &&
        (state.fvf_flags & fvf::POSITION_MASK) != fvf::XYZRHW) {
        for (u32 i = 0; i < FF_MAX_CLIP_PLANES; ++i) {
            if ((inputs.clip_plane_enable_mask & (1u << i)) != 0) {
                append_float4(out, inputs.clip_planes[i][0],
                              inputs.clip_planes[i][1],
                              inputs.clip_planes[i][2],
                              inputs.clip_planes[i][3]);
            } else {
                append_float4(out, 0.0f, 0.0f, 0.0f, 0.0f);
            }
        }
    }

    if (ff_lighting_uniforms(state)) {
        // Direct3D lights in camera space (Learn, "Camera Space
        // Transformations (Direct3D 9)"): the camera sits at the origin, and
        // with D3DRS_LOCALVIEWER FALSE the halfway vector is
        // norm((0,0,1) + Ldir) (Learn, "Specular Lighting (Direct3D 9)").
        // GL lighting (gl_spot_model) reads neither.
        append_float4(out, 0.0f, 0.0f, 0.0f, 1.0f);
        append_float4(out, 0.0f, 0.0f, 1.0f, 0.0f);
        append_float4(out, state.global_ambient[0], state.global_ambient[1],
                      state.global_ambient[2], state.global_ambient[3]);
        append_bytes(out, state.material.diffuse, sizeof(float) * 4);
        append_bytes(out, state.material.ambient, sizeof(float) * 4);
        append_bytes(out, state.material.specular, sizeof(float) * 4);
        append_bytes(out, state.material.emissive, sizeof(float) * 4);
        append_float4(out, state.material.power, 0.0f, 0.0f, 0.0f);

        for (u32 i = 0; i < state.active_light_count; ++i) {
            const FFLight& light = state.lights[i];
            append_bytes(out, light.diffuse, sizeof(float) * 4);
            append_bytes(out, light.specular, sizeof(float) * 4);
            append_bytes(out, light.ambient, sizeof(float) * 4);
            if (state.gl_spot_model) {
                // GL 1.1 spec 2.13.1 (ff_emulation.hpp gl_spot_model).
                append_float4(out, light.position[0], light.position[1], light.position[2],
                              light.gl_position_w);
                append_float4(out, light.direction[0], light.direction[1], light.direction[2],
                              light.gl_spot_exponent);
                // cos(90) is exactly 0 (a float cos(pi / 2) is not), so the
                // cone of a 90-degree spot is exactly the front half-space.
                const float cutoff_cos =
                    light.gl_spot_cutoff == 180.0f  ? -2.0f
                    : light.gl_spot_cutoff == 90.0f ? 0.0f
                                                    : std::cos(light.gl_spot_cutoff * 3.14159265358979323846f / 180.0f);
                append_float4(out, light.attenuation[0], light.attenuation[1], light.attenuation[2],
                              cutoff_cos);
                continue;
            }
            // D3DLIGHT9 Position and Direction are world space (Learn,
            // D3DLIGHT9); in camera space Lp * vMatrix (Learn, "Camera Space
            // Transformations (Direct3D 9)"), and a direction, w = 0, by the
            // same matrix.
            const auto camera = [&](const float* v, float w) {
                std::array<float, 4> r{};
                for (usize j = 0; j < 4; ++j) {
                    r[j] = v[0] * view[0 * 4 + j] + v[1] * view[1 * 4 + j] + v[2] * view[2 * 4 + j] +
                           w * view[3 * 4 + j];
                }
                return r;
            };
            const auto position = camera(light.position, 1.0f);
            const auto direction = camera(light.direction, 0.0f);
            append_float4(out, position[0], position[1], position[2],
                          static_cast<float>(light.type));
            append_float4(out, direction[0], direction[1], direction[2],
                          light.range);
            append_float4(out, light.attenuation[0], light.attenuation[1],
                          light.attenuation[2], light.spot_phi);
            if (light.type == LightType::Spot) {
                // D3DLIGHT9 Theta / Phi are the full cone angles; the runtime
                // halves them before the cosines (Learn, "Light Types
                // (Direct3D 9)").
                append_float4(out, std::cos(light.spot_theta * 0.5f),
                              std::cos(light.spot_phi * 0.5f),
                              light.spot_falloff, 0.0f);
            }
        }
    }

    if (state.fog_mode != FogMode::None) {
        append_float4(out, state.fog_start, state.fog_end, state.fog_density,
                      0.0f);
    }

    if (state.two_sided_lighting && ff_lighting_uniforms(state)) {
        const FFMaterial& m = state.back_material;
        append_bytes(out, m.diffuse, sizeof(float) * 4);
        append_bytes(out, m.ambient, sizeof(float) * 4);
        append_bytes(out, m.specular, sizeof(float) * 4);
        append_bytes(out, m.emissive, sizeof(float) * 4);
        append_float4(out, m.power, 0.0f, 0.0f, 0.0f);
    }
    if (ff_texgen_needs_planes(state)) {
        for (u32 unit = 0; unit < FF_MAX_TEXGEN_UNITS; ++unit) {
            for (u32 coord = 0; coord < 4; ++coord) {
                const FFTexGenMode mode = state.texgen_plane_mode[unit][coord];
                const auto& p = mode == FFTexGenMode::ObjectLinear ? inputs.texgen_object_planes[unit][coord]
                                : mode == FFTexGenMode::EyeLinear  ? inputs.texgen_eye_planes[unit][coord]
                                                                   : FFUniformInputs::Plane{};
                append_float4(out, p[0], p[1], p[2], p[3]);
            }
        }
    }
    if (state.window_offset) {
        // Window pixels to NDC (ff_emulation.hpp window_offset).
        const float sx = inputs.viewport_width > 0.0f ? 2.0f / inputs.viewport_width : 0.0f;
        const float sy = inputs.viewport_height > 0.0f ? 2.0f / inputs.viewport_height : 0.0f;
        append_float4(out, sx, sy, 0.0f, 0.0f);
    }
}

void pack_ff_fragment_uniforms(const FFPipelineState& state,
                               const FFUniformInputs& inputs,
                               std::vector<u8>& out) {
    out.clear();

    append_float4(out, inputs.texture_factor[0], inputs.texture_factor[1],
                  inputs.texture_factor[2], inputs.texture_factor[3]);

    // D3DTSS_CONSTANT is independent for every fixed-function texture stage.
    // Keep all eight slots in the dynamic uniform block: changing a constant
    // must not rebuild the pipeline, while each generated stage indexes its
    // own value.
    for (u32 stage = 0; stage < FF_MAX_TEXTURE_STAGES; ++stage) {
        const auto& constant = inputs.stage_constants[stage];
        append_float4(out, constant[0], constant[1], constant[2], constant[3]);
    }

    for (u32 stage = 0; stage < FF_MAX_TEXTURE_STAGES; ++stage) {
        const float bias = inputs.mipmap_lod_bias[stage];
        append_bytes(out, &bias, sizeof(bias));
    }

    if (state.fog_mode != FogMode::None) {
        append_float4(out, state.fog_color[0], state.fog_color[1],
                      state.fog_color[2], state.fog_color[3]);
        if (state.table_fog) {
            append_float4(out, state.fog_start, state.fog_end,
                          state.fog_density, 0.0f);
        }
    }
    if (state.alpha_test_enable) {
        // GL (alpha_ref_float): the float reference, clamped (spec 4.1.4).
        const float reference = state.alpha_ref_float
                                    ? std::clamp(inputs.alpha_reference, 0.0f, 1.0f)
                                    : static_cast<float>(state.alpha_ref) / 255.0f;
        append_bytes(out, &reference, sizeof(reference));
        // Metal requires the buffer size to cover the declared struct; the
        // trailing float is padded to the next 16-byte boundary.
        const std::array<float, 3> padding{};
        append_bytes(out, padding.data(), sizeof(padding));
    }

    // D3DTS_TEXTURE0..n — packed whenever any stage sets
    // TEXTURETRANSFORMFLAGS so the constant-buffer layout matches both
    // pure-FF and hybrid FF fragment shaders.
    bool need_tex_matrices = false;
    for (u32 i = 0; i < state.active_texture_stages; ++i) {
        if (state.texture_stages[i].texture_transform_flags != 0) {
            need_tex_matrices = true;
            break;
        }
    }
    if (need_tex_matrices) {
        // MSL value = D3DTS_TEXTUREn; applied as `tc * cb.texture_matrix`.
        for (u32 i = 0;
             i < std::min(state.active_texture_stages, FF_MAX_TEXTURE_STAGES);
             ++i) {
            append_matrix(out,
                          ff_matrix_transpose(inputs.texture_transforms[i]));
        }
    }

    if (state.line_stipple.enabled) {
        const std::array<u32, 4> stipple{std::clamp(state.line_stipple.factor, 1u, 256u),
                                         state.line_stipple.pattern, 0u, 0u};
        append_bytes(out, stipple.data(), sizeof(stipple));
    }
    if (state.polygon_stipple) {
        append_bytes(out, inputs.polygon_stipple_rows.data(), sizeof(inputs.polygon_stipple_rows));
    }
    if (state.gl_texture_sampling) {
        for (u32 i = 0; i < std::min(state.active_texture_stages, FF_MAX_TEXTURE_STAGES); ++i) {
            const FFUniformInputs::GlTextureUnit& u = inputs.gl_textures[i];
            const std::array<u32, 8> words{u.min_filter, u.mag_filter, u.wrap_s,    u.wrap_t,
                                           u.log2_width, u.log2_height, u.top_level, u.dimensions};
            append_bytes(out, words.data(), sizeof(words));
            append_float4(out, u.border[0], u.border[1], u.border[2], u.border[3]);
        }
    }
}

} // namespace vitreo::graphics
